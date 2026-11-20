#ifndef __DATABASE_COMMON_H__
#define __DATABASE_COMMON_H__

#include <string>

namespace db
{
	struct CConnectParam
	{
		CConnectParam(const std::string& strHost, unsigned int nPort, const std::string& strAccount, const std::string& strPasswd, const std::string& strDB, const std::string& strCharset);
		CConnectParam(const std::string& strParam, char delimiter);
		std::string m_host;
		unsigned int m_port{ 0 };
		std::string m_account;
		std::string m_password;
		std::string m_database;
		std::string m_charset;
	};

	enum class em_data_types
	{
		em_string = 0,
		em_int32,
		em_int64,
		em_double,
		em_bool,
		binary
	};

	enum class em_database
	{
		unknown = 0,
		mysql,
		oracle,
		sqlite
	};

	em_data_types GetDataType(em_database ty, int nType);
	std::string GetDBName(em_database ty);
	em_database GetDBType(const std::string& strName);
} // namespace db

#endif
