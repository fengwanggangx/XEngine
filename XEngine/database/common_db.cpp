#include "common_db.h"
#include <mysql/field_types.h>
#include <unordered_map>
#include "../common/utility.h"

namespace db
{
	CConnectParam::CConnectParam(const std::string& strHost, unsigned int nPort, const std::string& strAccount,
		const std::string& strPasswd, const std::string& strDB, const std::string& strCharset)
		: m_host(strHost), m_port(nPort), m_account(strAccount), m_password(strPasswd), m_database(strDB),
		  m_charset(strCharset)
	{
	}

	CConnectParam::CConnectParam(const std::string& strParam, char delimiter)
	{
		std::vector<std::string> data;
		if (utility::split(strParam, data, delimiter, false) == 6)
		{
			m_host = data[0];
			utility::to_number(data[1], m_port);
			m_account = data[2];
			m_password = data[3];
			m_database = data[4];
			m_charset = data[5];
		}
	}

	std::string GetDBName(em_database ty)
	{
		switch (ty)
		{
		case em_database::mysql:
			return "mysql";
		case em_database::oracle:
			return "oracle";
		case em_database::sqlite:
			return "sqlite";
		default:
			return "";
		}
	}

	em_database GetDBType(const std::string& strName)
	{
		if (strName == "mysql")
		{
			return em_database::mysql;
		}
		if (strName == "oracle")
		{
			return em_database::oracle;
		}
		if (strName == "sqlite")
		{
			return em_database::sqlite;
		}
		return em_database::unknown;
	}

	em_data_types GetDataType(em_database ty, int nType)
	{
		if (ty != em_database::mysql)
		{
			return em_data_types::em_string;
		}
		switch (nType)
		{
		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_INT24:
		case MYSQL_TYPE_ENUM:
			return em_data_types::em_int32;
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_LONGLONG:
			return em_data_types::em_int64;
		case MYSQL_TYPE_DECIMAL:
		case MYSQL_TYPE_FLOAT:
		case MYSQL_TYPE_DOUBLE:
			return em_data_types::em_double;
		case MYSQL_TYPE_BOOL:
			return em_data_types::em_bool;
		case MYSQL_TYPE_TINY_BLOB:
		case MYSQL_TYPE_MEDIUM_BLOB:
		case MYSQL_TYPE_LONG_BLOB:
		case MYSQL_TYPE_BLOB:
			return em_data_types::binary;
		default:
			return em_data_types::em_string;
		}
	}
} // namespace db
