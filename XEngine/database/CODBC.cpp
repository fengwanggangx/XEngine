#include "CODBC.h"
#include "CMySQL.h"
#include "COracle.h"
#include "CSQLite3.h"
#include <mutex>
#include <utility>

namespace db
{
	namespace
	{
		IDataBase* CreateDB(em_database t)
		{
			switch (t)
			{
			case em_database::sqlite:
			{
				return new CSQLite3();
			}
			case em_database::mysql:
			{
				return new CMySQL();
			}
			case em_database::oracle:
			{
				return new COracle();
			}
			default:
			{
				return nullptr;
			}
			}
		}

		int CloseDataBases(std::vector<std::unique_ptr<IDataBase>>& dbs)
		{
			int nResult = 0;
			for (const auto& v : dbs)
			{
				if ((nullptr != v) && (0 != v->Close()))
				{
					nResult = -1;
				}
			}
			return nResult;
		}
	} // namespace

	CODBC::CODBC()
	{
	}

	CODBC::~CODBC()
	{
		Close();
	}

	int CODBC::Connect(em_database t, const CConnectParam& param, std::size_t nCount)
	{
		if ((em_database::unknown == t) || (0 >= nCount))
		{
			return -1;
		}

		std::vector<std::unique_ptr<IDataBase>> dbs;
		dbs.reserve(nCount);

		for (std::size_t i = 0; i < nCount; ++i)
		{
			std::unique_ptr<IDataBase> db(CreateDB(t));
			if ((nullptr == db) || (0 != db->Connect(param)))
			{
				CloseDataBases(dbs);
				return -1;
			}
			dbs.emplace_back(std::move(db));
		}

		std::unique_lock<std::shared_mutex> lck(m_pool_mutex);
		auto& pool = m_pool[t];
		pool.reserve(pool.size() + dbs.size());
		for (auto& v : dbs)
		{
			pool.emplace_back(std::move(v));
		}
		return 0;
	}

	int CODBC::Close()
	{
		std::vector<std::unique_ptr<IDataBase>> dbs;
		bool bBusy = false;
		{
			std::unique_lock<std::shared_mutex> lock(m_pool_mutex);
			for (auto& item : m_pool)
			{
				auto& pool = item.second;
				for (auto vIter = pool.begin(); vIter != pool.end();)
				{
					auto& v = *vIter;
					if ((nullptr != v) && (status::busy == v->m_status))
					{
						bBusy = true;
						++vIter;
						continue;
					}
					dbs.emplace_back(std::move(v));
					vIter = pool.erase(vIter);
				}
			}
			for (auto mIter = m_pool.begin(); mIter != m_pool.end();)
			{
				if (mIter->second.empty())
				{
					mIter = m_pool.erase(mIter);
				}
				else
				{
					++mIter;
				}
			}
		}

		int nResult = CloseDataBases(dbs);
		return bBusy ? -1 : nResult;
	}

	int CODBC::Close(em_database t)
	{
		std::vector<std::unique_ptr<IDataBase>> dbs;
		bool bBusy = false;
		{
			std::unique_lock<std::shared_mutex> lock(m_pool_mutex);
			auto mIter = m_pool.find(t);
			if (m_pool.end() == mIter)
			{
				return 0;
			}

			auto& pool = mIter->second;
			for (auto iter = pool.begin(); iter != pool.end();)
			{
				if ((nullptr != *iter) && (status::busy == (*iter)->m_status))
				{
					bBusy = true;
					++iter;
					continue;
				}
				dbs.emplace_back(std::move(*iter));
				iter = pool.erase(iter);
			}
			if (pool.empty())
			{
				m_pool.erase(mIter);
			}
		}

		int nResult = CloseDataBases(dbs);
		return bBusy ? -1 : nResult;
	}

	_TyDBPtr CODBC::GetADataBase(em_database t)
	{
		IDataBase* pDB = nullptr;
		{
			std::unique_lock<std::shared_mutex> lock(m_pool_mutex);
			const auto mIter = m_pool.find(t);
			if (m_pool.end() != mIter)
			{
				for (const auto& v : mIter->second)
				{
					if ((nullptr != v) && (status::free == v->m_status))
					{
						v->m_status = status::busy;
						pDB = v.get();
						break;
					}
				}
			}
		}

		if (nullptr == pDB)
		{
			return nullptr;
		}

		_TyDBReleasor releasor = [this](IDataBase* pDatabase)
		{
			if (nullptr == pDatabase)
			{
				return;
			}
			std::unique_lock<std::shared_mutex> lck(m_pool_mutex);
			pDatabase->m_status = status::free;
		};
		return _TyDBPtr(pDB, std::move(releasor));
	}

	std::size_t CODBC::Count(em_database t) const
	{
		std::shared_lock<std::shared_mutex> lck(m_pool_mutex);
		const auto mIter = m_pool.find(t);
		return m_pool.end() == mIter ? 0 : mIter->second.size();
	}
} // namespace db
