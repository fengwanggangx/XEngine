#include "CSQLite3.h"
#include <memory>
#include <sqlite3.h>
#include <utility>

namespace db
{
	CSQLite3::CSQLite3()
	{
	}

	CSQLite3::~CSQLite3()
	{
		Close();
	}

	int CSQLite3::Connect(const CConnectParam& param)
	{
		if (nullptr != m_db)
		{
			return SQLITE_OK;
		}

		sqlite3* pDB = nullptr;
		int nRet = sqlite3_open_v2(param.m_database.c_str(), &pDB, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
		if (SQLITE_OK != nRet)
		{
			if (nullptr != pDB)
			{
				sqlite3_close(pDB);
			}
			return nRet;
		}

		m_db = pDB;
		return SQLITE_OK;
	}

	int CSQLite3::Close()
	{
		if (nullptr == m_db)
		{
			return SQLITE_OK;
		}

		int nRet = sqlite3_close(static_cast<sqlite3*>(m_db));
		if (SQLITE_OK == nRet)
		{
			m_db = nullptr;
		}
		return nRet;
	}

	int CSQLite3::ExecUpdate(const std::string& strSQL)
	{
		if (nullptr == m_db)
		{
			return SQLITE_MISUSE;
		}
		return sqlite3_exec(static_cast<sqlite3*>(m_db), strSQL.c_str(), nullptr, nullptr, nullptr);
	}

	const CTable& CSQLite3::ExecQuery(const std::string& strSQL)
	{
		thread_local CTable table;
		table.Clear();
		if (nullptr == m_db)
		{
			return table;
		}

		sqlite3_stmt* pStatement = nullptr;
		int nRet = sqlite3_prepare_v2(static_cast<sqlite3*>(m_db), strSQL.c_str(), -1, &pStatement, nullptr);
		auto statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>(pStatement, sqlite3_finalize);
		if (SQLITE_OK != nRet)
		{
			return table;
		}

		std::size_t nCols = static_cast<std::size_t>(sqlite3_column_count(statement.get()));
		table.m_columns.reserve(nCols);
		for (std::size_t i = 0; i < nCols; ++i)
		{
			table.m_columns.emplace_back();
			CColumn& column = table.m_columns.back();
			column.m_id = i;
			column.m_name = sqlite3_column_name(statement.get(), i);
		}

		while (SQLITE_ROW == sqlite3_step(statement.get()))
		{
			table.m_rows.emplace_back();
			_TyRow& row = table.m_rows.back();
			row.reserve(nCols);
			for (std::size_t i = 0; i < nCols; ++i)
			{
				const unsigned char* pszValue = sqlite3_column_text(statement.get(), i);
				row.emplace_back(nullptr == pszValue ? "" : reinterpret_cast<const char*>(pszValue));
			}
		}
		return table;
	}

	bool CSQLite3::BeginTransaction()
	{
		return SQLITE_OK == ExecUpdate("BEGIN IMMEDIATE");
	}

	bool CSQLite3::EndTransaction()
	{
		return SQLITE_OK == ExecUpdate("COMMIT");
	}

	bool CSQLite3::RollBackTransaction()
	{
		return SQLITE_OK == ExecUpdate("ROLLBACK");
	}
} // namespace db
