#include "CMySQL.h"
#include <unordered_map>
#include <mysql/mysql.h>
#include "common_db.h"
#include <iostream>
#include <memory>

namespace db
{
	CMySQL::CMySQL()
	{
	}

	CMySQL::~CMySQL()
	{
		Close();
	}

	int CMySQL::Connect(const db::CConnectParam& param)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (IsValid())
		{
			return 0;
		}
		if (param.m_strDataBase.empty() || (std::string::npos != param.m_strDataBase.find('\0')))
		{
			std::cerr << "MySQL database name is invalid\n";
			return 1;
		}
		if (nullptr == m_pDB)
		{
			m_pDB = mysql_init(nullptr);
			if (nullptr == m_pDB)
			{
				return 1;
			}
		}

		mysql_options(m_pDB, MYSQL_SET_CHARSET_NAME, param.m_strCharset.c_str());
		if (nullptr == mysql_real_connect(m_pDB, param.m_strHost.c_str(), param.m_strAccount.c_str(), param.m_strPasswd.c_str(), nullptr, param.m_nPort, nullptr, 0))
		{
			std::cerr << "MySQL connection failed: " << mysql_error(m_pDB) << '\n';
			mysql_close(m_pDB);
			m_pDB = nullptr;
			return 1;
		}
		std::string strSQL = "CREATE DATABASE IF NOT EXISTS `" + param.m_strDataBase + "` CHARACTER SET utf8mb4";
		if ((0 != mysql_query(m_pDB, strSQL.c_str())) || (0 != mysql_select_db(m_pDB, param.m_strDataBase.c_str())))
		{
			std::cerr << "MySQL database initialization failed: " << mysql_error(m_pDB) << '\n';
			mysql_close(m_pDB);
			m_pDB = nullptr;
			return 1;
		}
		return 0;
	}

	void CMySQL::DisConnect()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (m_pDB != nullptr)
		{
			mysql_close(m_pDB);
		}
		m_pDB = nullptr;
	}

	bool CMySQL::IsValid()
	{
		return (nullptr != m_pDB) && (0 == mysql_ping(m_pDB));
	}

	int CMySQL::Close()
	{
		DisConnect();
		return 0;
	}

	const CTable& CMySQL::ExecQuery(const std::string& strSQL)
	{
		thread_local CTable s_table;
		std::lock_guard<std::mutex> lock(m_mtx);

		CTable::_TyColumns& columns = s_table.m_columns;
		CTable::_TyRows& rows = s_table.m_rows;

		columns.clear();
		rows.clear();

		if (!IsValid())
		{
			return s_table;
		}
		int nRet = mysql_query(m_pDB, strSQL.c_str());
		if (0 != nRet)
		{
			return s_table;
		}

		std::unique_ptr<MYSQL_RES, void (*)(MYSQL_RES*)> result(mysql_store_result(m_pDB), [](MYSQL_RES* p)
																{
																	if (nullptr != p)
																	{
																		mysql_free_result(p);
																	}
																});
		if (nullptr == result.get())
		{
			//(0 == mysql_field_count(m_pDB))
			return s_table;
		}

		int nFields = mysql_num_fields(result.get());
		MYSQL_FIELD* pFields = mysql_fetch_fields(result.get());

		columns.reserve(nFields);
		for (int i = 0; i < nFields; ++i)
		{
			columns.emplace_back();
			CColumn& col = columns.back();
			col.m_uId = static_cast<unsigned int>(i);
			col.m_strName = pFields[i].name;
			col.m_type = db::GetDataType(em_database::mysql, pFields[i].type);
		}
		uint64_t nRows = mysql_num_rows(result.get());

		rows.reserve(nRows);
		MYSQL_ROW row;
		while ((row = mysql_fetch_row(result.get())))
		{
			_TyRow rowData;
			rowData.reserve(nFields);
			unsigned long* lengths = mysql_fetch_lengths(result.get());

			for (int i = 0; i < nFields; ++i)
			{
				if (row[i])
				{
					rowData.emplace_back(std::string(row[i], lengths[i]));
				}
				else
				{
					rowData.emplace_back("");
				}
			}

			rows.emplace_back(std::move(rowData));
		}
		return s_table;
	}

	int CMySQL::ExecUpdate(const std::string& strSQL)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!IsValid())
		{
			return -1;
		}
		int nRet = mysql_query(m_pDB, strSQL.c_str());
		if (0 != nRet)
		{
		}
		return nRet;
	}

	int CMySQL::ExecScript(const std::string& strSQL)
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!IsValid())
		{
			return -1;
		}
		if (0 != mysql_set_server_option(m_pDB, MYSQL_OPTION_MULTI_STATEMENTS_ON))
		{
			return mysql_errno(m_pDB);
		}

		int nRet = mysql_real_query(m_pDB, strSQL.data(), static_cast<unsigned long>(strSQL.size()));
		if (0 == nRet)
		{
			int nNextResult = 0;
			do
			{
				MYSQL_RES* pResult = mysql_store_result(m_pDB);
				if (nullptr != pResult)
				{
					mysql_free_result(pResult);
				}
				else if ((0 != mysql_field_count(m_pDB)) && (0 == nRet))
				{
					nRet = mysql_errno(m_pDB);
				}

				nNextResult = mysql_next_result(m_pDB);
				if ((0 < nNextResult) && (0 == nRet))
				{
					nRet = mysql_errno(m_pDB);
				}
			}
			while (0 == nNextResult);
		}

		int nDisableRet = mysql_set_server_option(m_pDB, MYSQL_OPTION_MULTI_STATEMENTS_OFF);
		if ((0 == nRet) && (0 != nDisableRet))
		{
			nRet = mysql_errno(m_pDB);
		}
		return nRet;
	}

	bool CMySQL::BeginTransaction()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!IsValid())
		{
			return false;
		}
		return 0 == mysql_autocommit(m_pDB, 0);
	}

	bool CMySQL::EndTransaction()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!IsValid())
		{
			return false;
		}
		bool bRet = 0 == mysql_commit(m_pDB);
		mysql_autocommit(m_pDB, 1);
		return bRet;
	}

	bool CMySQL::RollBackTransaction()
	{
		std::lock_guard<std::mutex> lock(m_mtx);
		if (!IsValid())
		{
			return false;
		}
		bool bRet = 0 == mysql_rollback(m_pDB);
		mysql_autocommit(m_pDB, 1);
		return bRet;
	}

} // namespace db
