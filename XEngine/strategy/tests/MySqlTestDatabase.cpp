#include "MySqlTestDatabase.h"
#include "../../database/CMySQL.h"
#include "../../common/utility.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace
{
	std::set<std::string> databases;

	std::string Environment(const char* key, const std::string& fallback = {})
	{
		const char* value = std::getenv(key);
		return nullptr == value ? fallback : value;
	}

	db::CConnectParam Parameters(const std::string& name)
	{
		unsigned int port = 0;
		if (!utility::to_number(Environment("XENGINE_TEST_MYSQL_PORT", "3306"), port))
		{
			throw std::runtime_error("Invalid MySQL test port");
		}
		return { Environment("XENGINE_TEST_MYSQL_HOST"), port, Environment("XENGINE_TEST_MYSQL_USER", "root"), Environment("XENGINE_TEST_MYSQL_PASSWORD"), name, "utf8mb4" };
	}
} // namespace

bool HasTestMySql()
{
	return !Environment("XENGINE_TEST_MYSQL_HOST").empty();
}

std::string NewTestDatabaseName()
{
	static std::atomic_uint64_t sequence{ 0 };
	static std::string prefix = "xe_strategy_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_";
	return prefix + std::to_string(sequence.fetch_add(1));
}

db::_TyDBPtr OpenTestDatabase(const std::string& name)
{
	std::string database = name.empty() ? NewTestDatabaseName() : name;
	if (!database.starts_with("xe_strategy_test_") || !std::all_of(database.begin(), database.end(), [](char value)
	{
		return ('_' == value) || (('a' <= value) && ('z' >= value)) || (('0' <= value) && ('9' >= value));
	}))
	{
		throw std::runtime_error("Refusing non-test database");
	}
	db::_TyDBPtr connection(new db::CMySQL(), [](db::IDataBase* value)
	{
		delete value;
	});
	if (0 != connection->Connect(Parameters(database)))
	{
		throw std::runtime_error("Cannot connect isolated MySQL test database");
	}
	databases.emplace(database);
	std::ifstream input(XENGINE_TEST_SCHEMA);
	std::string sql{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
	std::size_t begin = sql.find("-- Durable strategy execution state");
	if ((std::string::npos == begin) || (0 != connection->ExecScript(sql.substr(begin))))
	{
		throw std::runtime_error("Cannot initialize MySQL strategy test tables");
	}
	return connection;
}

CTestDatabases::~CTestDatabases()
{
	for (const auto& name : databases)
	{
		try
		{
			db::CMySQL connection;
			if ((0 != connection.Connect(Parameters(name))) || (0 != connection.ExecUpdate("DROP DATABASE `" + name + "`")))
			{
				std::cerr << "MySQL test schema cleanup failed\n";
			}
		}
		catch (...)
		{
			std::cerr << "MySQL test schema cleanup failed\n";
		}
	}
}
