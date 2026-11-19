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
	if (m_bInitialized)
	{
		return true;
	}

	if (nullptr != m_p_tcp_server || nullptr != m_p_http_server || nullptr != m_p_hq_market || nullptr != m_pBroker || nullptr != m_p_account_service || nullptr != m_p_trade_service || nullptr != m_p_strategy_service || nullptr != m_p_engine_center || nullptr != m_p_hq_service)
	{
		Finalize();
	}

	m_exec = std::filesystem::current_path();

	m_n_error_code = 0;
	m_str_last_error.clear();
	net::EnvInitialize();
	if (!net::IsThreadEnable())
	{
		m_n_error_code = 1;
		m_str_last_error = "Failed to enable libevent thread support";
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
		m_n_error_code = 2;
		m_str_last_error = "HQMarket mysql param error";
		return false;
	}

	db::CConnectParam dbParam(strMySqlHost, static_cast<unsigned int>(nMySqlPort), strMySqlAccount, strMySqlPassword, strMySqlDatabase, "utf8mb4");
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, nMySqlPoolSize))
	{
		m_n_error_code = 3;
		m_str_last_error = "MySQL initialization failed";
		return false;
	}
	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if ((nullptr == db) || (0 != db->ExecSqlFile(m_exec / "sql" / "table_create.sql")))
	{
		m_n_error_code = 4;
		m_str_last_error = "Failed to initialize MySQL tables";
		return false;
	}
	if (0 != db->ExecSqlFile(m_exec / "sql" / "table_init.sql"))
	{
		m_n_error_code = 5;
		m_str_last_error = "Failed to execute sql/table_init.sql";
		return false;
	}

	// Keep the configured business pool capacity available after reserving execution's session.
	db.reset();
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, 1))
	{
		m_n_error_code = 3;
		m_str_last_error = "Failed to reserve MySQL strategy execution connection";
		return false;
	}

	int nTcpPort = hIni.GetInt(ini::Config::System, "System", "tcp_port");
	int nHttpPort = hIni.GetInt(ini::Config::System, "System", "http_port");
	if ((nTcpPort <= 0) || (nHttpPort <= 0))
	{
		m_n_error_code = 6;
		m_str_last_error = "HQMarket tcp_port && http_port is required in ini/system.ini";
		return false;
	}

	m_p_tcp_server = new net::CTcpServer(nTcpPort);
	m_p_http_server = new net::CHttpServer(nHttpPort);

	static const std::string cs_hqmarket = "HQMarket";
	CLoginInfo hq;
	hq.m_strUser = hIni.GetString(ini::Config::System, cs_hqmarket, "user");
	hq.m_strPassword = hIni.GetString(ini::Config::System, cs_hqmarket, "password");
	hq.m_host.m_strName = cs_hqmarket;
	hq.m_host.m_strHost = hIni.GetString(ini::Config::System, cs_hqmarket, "host");
	hq.m_host.m_nPort = hIni.GetInt(ini::Config::System, cs_hqmarket, "port");
	m_p_hq_market = new CSession(hq);

	static const std::string cs_broker = "Broker";
	CLoginInfo broker;
	broker.m_strUser = hIni.GetString(ini::Config::System, cs_broker, "user");
	broker.m_strPassword = hIni.GetString(ini::Config::System, cs_broker, "password");
	broker.m_host.m_strName = cs_broker;
	broker.m_host.m_strHost = hIni.GetString(ini::Config::System, cs_broker, "host");
	broker.m_host.m_nPort = hIni.GetInt(ini::Config::System, cs_broker, "port");
	if (!broker.Valid())
	{
		m_n_error_code = 7;
		m_str_last_error = "Broker endpoint and login credentials are required in ini/system.ini";
		Finalize();
		return false;
	}
	broker.m_bOrdered = true;
	m_pBroker = new CSession(broker);

	m_p_account_service = new CAccountService(m_pBroker);
	if (!m_p_account_service->Initialize())
	{
		m_n_error_code = 8;
		m_str_last_error = m_p_account_service->GetLastError();
		return false;
	}
	m_p_trade_service = new CTradeService(m_p_account_service, m_pBroker);
	if (!m_p_trade_service->Initialize())
	{
		m_n_error_code = 9;
		m_str_last_error = m_p_trade_service->GetLastError();
		return false;
	}
	if (!m_indicator.Initialize(m_str_last_error))
	{
		m_n_error_code = 10;
		Finalize();
		return false;
	}
	m_p_hq_service = new CHQService(m_p_hq_market);
	m_p_strategy_service = new CStrategyService(m_p_hq_service, m_p_trade_service, m_p_account_service, m_indicator);
	if (!m_p_strategy_service->Initialize())
	{
		m_n_error_code = 11;
		m_str_last_error = m_p_strategy_service->GetLastError();
		Finalize();
		return false;
	}
	m_p_engine_center = new CEngineCenter(m_p_tcp_server, m_p_hq_market, m_pBroker, m_p_hq_service, m_p_strategy_service, m_p_trade_service, m_p_account_service);
	if (!m_p_engine_center->Initialize())
	{
		m_n_error_code = 12;
		m_str_last_error = "Engine center initialization failed";
		Finalize();
		return false;
	}
	m_p_account_service->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, m_p_trade_service));
	if (!m_pBroker->Start())
	{
		m_n_error_code = 13;
		m_str_last_error = "Broker session initialization failed";
		Finalize();
		return false;
	}
	m_bInitialized = true;
	return true;
}

bool CBootLoader::Run()
{
	if (!m_bInitialized || (nullptr == m_p_tcp_server) || (nullptr == m_p_http_server))
	{
		m_n_error_code = 14;
		m_str_last_error = "Boot loader is not initialized";
		return false;
	}
	if (0 != m_p_tcp_server->Initialize())
	{
		m_n_error_code = 15;
		m_str_last_error = "TCP server initialization failed";
		return false;
	}
	if (0 != m_p_http_server->Initialize())
	{
		m_n_error_code = 16;
		m_str_last_error = "HTTP server initialization failed";
		return false;
	}
	if (!m_p_hq_market->Start())
	{
		m_n_error_code = 17;
		m_str_last_error = "HQMarket session configuration is invalid";
		return false;
	}
	std::jthread tcpServerThread(std::bind_front(&net::CTcpServer::Start, m_p_tcp_server, true));
	m_p_http_server->Start(true);
	m_p_tcp_server->ShutDown();
	return true;
}

void CBootLoader::Finalize()
{
	if (nullptr != m_p_http_server)
	{
		m_p_http_server->ShutDown();
	}
	if (nullptr != m_p_tcp_server)
	{
		m_p_tcp_server->ShutDown();
	}
	// Quiesce request callbacks before stopping the services they borrow.
	if (nullptr != m_p_tcp_server)
	{
		m_p_tcp_server->Drain();
		m_p_tcp_server->ClearHandlers();
	}
	if (nullptr != m_p_strategy_service)
	{
		m_p_strategy_service->StopAll();
	}
	if (nullptr != m_p_hq_market)
	{
		m_p_hq_market->Stop();
	}
	if (nullptr != m_pBroker)
	{
		m_pBroker->Stop();
	}
	if (nullptr != m_p_trade_service)
	{
		m_p_trade_service->Stop();
	}
	if (nullptr != m_p_account_service)
	{
		m_p_account_service->Stop();
	}
	if (nullptr != m_pBroker)
	{
		m_pBroker->ClearHandlers();
	}
	if (nullptr != m_p_hq_market)
	{
		m_p_hq_market->ClearHandlers();
	}
	// All callback producers have stopped; borrowers can now be destroyed.
	delete m_p_engine_center;
	m_p_engine_center = nullptr;
	delete m_p_strategy_service;
	m_p_strategy_service = nullptr;
	m_indicator.Finalize();
	delete m_p_hq_service;
	m_p_hq_service = nullptr;
	delete m_p_trade_service;
	m_p_trade_service = nullptr;
	delete m_p_account_service;
	m_p_account_service = nullptr;
	delete m_pBroker;
	m_pBroker = nullptr;
	delete m_p_hq_market;
	m_p_hq_market = nullptr;
	delete m_p_http_server;
	m_p_http_server = nullptr;
	delete m_p_tcp_server;
	m_p_tcp_server = nullptr;
	CDBEngine::InstanceRef().Close();
	m_bInitialized = false;
}

net::CTcpServer& CBootLoader::GetTcpServer()
{
	return *m_p_tcp_server;
}

net::CHttpServer& CBootLoader::GetHttpServer()
{
	return *m_p_http_server;
}

const std::string& CBootLoader::GetLastError() const
{
	return m_str_last_error;
}

int CBootLoader::GetErrorCode() const
{
	return m_n_error_code;
}
