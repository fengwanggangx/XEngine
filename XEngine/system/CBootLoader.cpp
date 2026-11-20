#include "CBootLoader.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../ini/CINIHandler.h"
#include "../network/CHttpServer.h"
#include "../network/CTcpServer.h"
#include "CSession.h"
#include "CEngineCenter.h"
#include "CHQService.h"
#include "../strategy/CTradeService.h"
#include "../strategy/CAccountService.h"
#include "../strategy/CStrategyService.h"
#include <cstdlib>
#include <utility>
#include <functional>

namespace net
{
	void EnvInitialize();
	bool IsThreadEnable();
} // namespace net

CBootLoader::CBootLoader() = default;

CBootLoader::~CBootLoader()
{
	Finalize();
}

bool CBootLoader::Initialize()
{
	if (m_initialized)
	{
		return true;
	}

	if (nullptr != m_tcp_server || nullptr != m_http_server || nullptr != m_hq_market || nullptr != m_broker || nullptr != m_account_service || nullptr != m_trade_service || nullptr != m_strategy_service || nullptr != m_engine_center || nullptr != m_hq_service)
	{
		Finalize();
	}

	m_exec = std::filesystem::current_path();

	m_error_code = 0;
	m_last_error.clear();
	net::EnvInitialize();
	if (!net::IsThreadEnable())
	{
		m_error_code = 1;
		m_last_error = "Failed to enable libevent thread support";
		return false;
	}

	ini::CINIHandler& hIni = ini::CINIHandler::InstanceRef();
	static const std::string cs_mysql = "MySQL";
	std::string strMySqlHost = hIni.GetString(ini::Config::System, cs_mysql, "host");
	int nMySqlPort = hIni.GetInt(ini::Config::System, cs_mysql, "port");

	std::string strMySqlAccount = hIni.GetString(ini::Config::System, cs_mysql, "user");
	std::string strMySqlPassword = hIni.GetString(ini::Config::System, cs_mysql, "password");
	std::string strMySqlDatabase = hIni.GetString(ini::Config::System, cs_mysql, "database");

	int nMySqlPoolSize = hIni.GetInt(ini::Config::System, cs_mysql, "pool_size");

	if (strMySqlHost.empty() || (nMySqlPort <= 0) || strMySqlAccount.empty() || strMySqlPassword.empty() || strMySqlDatabase.empty() || (nMySqlPoolSize <= 0))
	{
		m_error_code = 2;
		m_last_error = "HQMarket mysql param error";
		return false;
	}

	db::CConnectParam dbParam(strMySqlHost, static_cast<unsigned int>(nMySqlPort), strMySqlAccount, strMySqlPassword, strMySqlDatabase, "utf8mb4");
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, nMySqlPoolSize))
	{
		m_error_code = 3;
		m_last_error = "MySQL initialization failed";
		return false;
	}
	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if ((nullptr == db) || (0 != db->ExecSqlFile(m_exec / "sql" / "table_create.sql")))
	{
		m_error_code = 4;
		m_last_error = "Failed to initialize MySQL tables";
		return false;
	}
	if (0 != db->ExecSqlFile(m_exec / "sql" / "table_init.sql"))
	{
		m_error_code = 5;
		m_last_error = "Failed to execute sql/table_init.sql";
		return false;
	}

	// Keep the configured business pool capacity available after reserving execution's session.
	db.reset();
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, 1))
	{
		m_error_code = 3;
		m_last_error = "Failed to reserve MySQL strategy execution connection";
		return false;
	}

	int nTcpPort = hIni.GetInt(ini::Config::System, "System", "tcp_port");
	int nHttpPort = hIni.GetInt(ini::Config::System, "System", "http_port");
	if ((nTcpPort <= 0) || (nHttpPort <= 0))
	{
		m_error_code = 6;
		m_last_error = "HQMarket tcp_port && http_port is required in ini/system.ini";
		return false;
	}

	m_tcp_server = new net::CTcpServer(nTcpPort);
	m_http_server = new net::CHttpServer(nHttpPort);

	static const std::string cs_hqmarket = "HQMarket";
	CLoginInfo hq;
	hq.m_user = hIni.GetString(ini::Config::System, cs_hqmarket, "user");
	hq.m_password = hIni.GetString(ini::Config::System, cs_hqmarket, "password");
	hq.m_host.m_name = cs_hqmarket;
	hq.m_host.m_host = hIni.GetString(ini::Config::System, cs_hqmarket, "host");
	hq.m_host.m_port = hIni.GetInt(ini::Config::System, cs_hqmarket, "port");
	m_hq_market = new CSession(hq);

	static const std::string cs_broker = "Broker";
	CLoginInfo broker;
	broker.m_user = hIni.GetString(ini::Config::System, cs_broker, "user");
	broker.m_password = hIni.GetString(ini::Config::System, cs_broker, "password");
	broker.m_host.m_name = cs_broker;
	broker.m_host.m_host = hIni.GetString(ini::Config::System, cs_broker, "host");
	broker.m_host.m_port = hIni.GetInt(ini::Config::System, cs_broker, "port");
	if (!broker.Valid())
	{
		m_error_code = 7;
		m_last_error = "Broker endpoint and login credentials are required in ini/system.ini";
		Finalize();
		return false;
	}
	broker.m_ordered = true;
	m_broker = new CSession(broker);

	m_account_service = new CAccountService(m_broker);
	if (!m_account_service->Initialize())
	{
		m_error_code = 8;
		m_last_error = m_account_service->GetLastError();
		return false;
	}
	m_trade_service = new CTradeService(m_account_service, m_broker);
	if (!m_trade_service->Initialize())
	{
		m_error_code = 9;
		m_last_error = m_trade_service->GetLastError();
		return false;
	}
	if (!m_indicator.Initialize(m_last_error))
	{
		m_error_code = 10;
		Finalize();
		return false;
	}
	m_hq_service = new CHQService(m_hq_market);
	m_strategy_service = new CStrategyService(m_hq_service, m_trade_service, m_account_service, m_indicator);
	if (!m_strategy_service->Initialize())
	{
		m_error_code = 11;
		m_last_error = m_strategy_service->GetLastError();
		Finalize();
		return false;
	}
	m_engine_center = new CEngineCenter(m_tcp_server, m_hq_market, m_broker, m_hq_service, m_strategy_service, m_trade_service, m_account_service);
	if (!m_engine_center->Initialize())
	{
		m_error_code = 12;
		m_last_error = "Engine center initialization failed";
		Finalize();
		return false;
	}
	m_account_service->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, m_trade_service));
	if (!m_broker->Start())
	{
		m_error_code = 13;
		m_last_error = "Broker session initialization failed";
		Finalize();
		return false;
	}
	m_initialized = true;
	return true;
}

bool CBootLoader::Run()
{
	if (!m_initialized || (nullptr == m_tcp_server) || (nullptr == m_http_server))
	{
		m_error_code = 14;
		m_last_error = "Boot loader is not initialized";
		return false;
	}
	if (0 != m_tcp_server->Initialize())
	{
		m_error_code = 15;
		m_last_error = "TCP server initialization failed";
		return false;
	}
	if (0 != m_http_server->Initialize())
	{
		m_error_code = 16;
		m_last_error = "HTTP server initialization failed";
		return false;
	}
	if (!m_hq_market->Start())
	{
		m_error_code = 17;
		m_last_error = "HQMarket session configuration is invalid";
		return false;
	}
	std::jthread tcpServerThread(std::bind_front(&net::CTcpServer::Start, m_tcp_server, true));
	m_http_server->Start(true);
	m_tcp_server->ShutDown();
	return true;
}

void CBootLoader::Finalize()
{
	if (nullptr != m_http_server)
	{
		m_http_server->ShutDown();
	}
	if (nullptr != m_tcp_server)
	{
		m_tcp_server->ShutDown();
	}
	// Quiesce request callbacks before stopping the services they borrow.
	if (nullptr != m_tcp_server)
	{
		m_tcp_server->Drain();
		m_tcp_server->ClearHandlers();
	}
	if (nullptr != m_strategy_service)
	{
		m_strategy_service->StopAll();
	}
	if (nullptr != m_hq_market)
	{
		m_hq_market->Stop();
	}
	if (nullptr != m_broker)
	{
		m_broker->Stop();
	}
	if (nullptr != m_trade_service)
	{
		m_trade_service->Stop();
	}
	if (nullptr != m_account_service)
	{
		m_account_service->Stop();
	}
	if (nullptr != m_broker)
	{
		m_broker->ClearHandlers();
	}
	if (nullptr != m_hq_market)
	{
		m_hq_market->ClearHandlers();
	}
	// All callback producers have stopped; borrowers can now be destroyed.
	delete m_engine_center;
	m_engine_center = nullptr;
	delete m_strategy_service;
	m_strategy_service = nullptr;
	m_indicator.Finalize();
	delete m_hq_service;
	m_hq_service = nullptr;
	delete m_trade_service;
	m_trade_service = nullptr;
	delete m_account_service;
	m_account_service = nullptr;
	delete m_broker;
	m_broker = nullptr;
	delete m_hq_market;
	m_hq_market = nullptr;
	delete m_http_server;
	m_http_server = nullptr;
	delete m_tcp_server;
	m_tcp_server = nullptr;
	CDBEngine::InstanceRef().Close();
	m_initialized = false;
}

net::CTcpServer& CBootLoader::GetTcpServer()
{
	return *m_tcp_server;
}

net::CHttpServer& CBootLoader::GetHttpServer()
{
	return *m_http_server;
}

const std::string& CBootLoader::GetLastError() const
{
	return m_last_error;
}

int CBootLoader::GetErrorCode() const
{
	return m_error_code;
}
