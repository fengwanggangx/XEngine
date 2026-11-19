#ifndef __IDATABASE_H__
#define __IDATABASE_H__

#include <filesystem>
#include <string>
#include <vector>
#include "CQueryTable.h"

namespace db
{
	enum class status
	{
		free,
		busy
	};

	class IDataBase
	{
	public:
		virtual ~IDataBase() = default;

	public:
		virtual int Connect(const CConnectParam& param) = 0;
		virtual int Close() = 0;

		// virtual int ReConnect(const std::string& strFile) = 0;

		virtual int ExecUpdate(const std::string& strSQL) = 0;
		virtual int ExecScript(const std::string& strSQL);
		virtual const CTable& ExecQuery(const std::string& strSQL) = 0;

		int ExecSqlFile(const std::filesystem::path& filePath);

		virtual bool BeginTransaction() = 0;
		virtual bool EndTransaction() = 0;
		virtual bool RollBackTransaction() = 0;

		virtual bool Transaction(const std::string& strSQL);

	public:
		db::status m_status{ db::status::free };
	};
} // namespace db
#endif
