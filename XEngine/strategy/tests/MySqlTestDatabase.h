#pragma once
#include "../../database/CODBC.h"
#include <string>

bool HasTestMySql();
std::string NewTestDatabaseName();
db::_TyDBPtr OpenTestDatabase(const std::string& name = {});

class CTestDatabases final
{
public:
	~CTestDatabases();
};
