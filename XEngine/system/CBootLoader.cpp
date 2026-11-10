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

	if (nullptr != m_pTcpServer || nullptr != m_pHttpServer || nullptr != m_pHQMarket || nullptr != m_pBroker || nullptr != m_pAccountService || nullptr != m_pTradeService || nullptr != m_pStrategyService || nullptr != m_pEngineCenter || nullptr != m_pHQService)
	{
		Finalize();
	}

	m_exec = std::filesystem::current_path();

	m_nErrorCode = 0;
	m_strLastError.clear();
	net::EnvInitialize();
	if (!net::IsThreadEnable())
	{
		m_nErrorCode = 1;
		m_strLastError = "Failed to enable libevent thread support";
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
		m_nErrorCode = 2;
		m_strLastError = "HQMarket mysql param error";
		return false;
	}

	db::CConnectParam dbParam(strMySqlHost, static_cast<unsigned int>(nMySqlPort), strMySqlAccount, strMySqlPassword, strMySqlDatabase, "utf8mb4");
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, nMySqlPoolSize))
	{
		m_nErrorCode = 3;
		m_strLastError = "MySQL initialization failed";
		return false;
	}
	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if ((nullptr == db) || (0 != db->ExecSqlFile(m_exec / "sql" / "table_create.sql")))
	{
		m_nErrorCode = 4;
		m_strLastError = "Failed to initialize MySQL tables";
		return false;
	}
	if (0 != db->ExecSqlFile(m_exec / "sql" / "table_init.sql"))
	{
		m_nErrorCode = 5;
		m_strLastError = "Failed to execute sql/table_init.sql";
		return false;
	}

	int nTcpPort = hIni.GetInt(ini::Config::System, "System", "tcp_port");
	int nHttpPort = hIni.GetInt(ini::Config::System, "System", "http_port");
	if ((nTcpPort <= 0) || (nHttpPort <= 0))
	{
		m_nErrorCode = 6;
		m_strLastError = "HQMarket tcp_port && http_port is required in ini/system.ini";
		return false;
	}

	m_pTcpServer = new net::CTcpServer(nTcpPort);
	m_pHttpServer = new net::CHttpServer(nHttpPort);

	static const std::string cs_hqmarket = "HQMarket";
	CLoginInfo hq;
	hq.m_strUser = hIni.GetString(ini::Config::System, cs_hqmarket, "user");
	hq.m_strPassword = hIni.GetString(ini::Config::System, cs_hqmarket, "password");
	hq.m_host.m_strName = cs_hqmarket;
	hq.m_host.m_strHost = hIni.GetString(ini::Config::System, cs_hqmarket, "host");
	hq.m_host.m_nPort = hIni.GetInt(ini::Config::System, cs_hqmarket, "port");
	m_pHQMarket = new CSession(hq);

	static const std::string cs_broker = "Broker";
	CLoginInfo broker;
	broker.m_strUser = hIni.GetString(ini::Config::System, cs_broker, "user");
	broker.m_strPassword = hIni.GetString(ini::Config::System, cs_broker, "password");
	broker.m_host.m_strName = cs_broker;
	broker.m_host.m_strHost = hIni.GetString(ini::Config::System, cs_broker, "host");
	broker.m_host.m_nPort = hIni.GetInt(ini::Config::System, cs_broker, "port");
	if (!broker.Valid())
	{
		m_nErrorCode = 7;
		m_strLastError = "Broker endpoint and login credentials are required in ini/system.ini";
		Finalize();
		return false;
	}
	broker.m_bOrdered = true;
	m_pBroker = new CSession(broker);

	m_pAccountService = new CAccountService(m_pBroker);
	if (!m_pAccountService->Initialize())
	{
		m_nErrorCode = 8;
		m_strLastError = m_pAccountService->GetLastError();
		return false;
	}
	m_pTradeService = new CTradeService(m_pAccountService, m_pBroker);
	if (!m_pTradeService->Initialize())
	{
		m_nErrorCode = 9;
		m_strLastError = m_pTradeService->GetLastError();
		return false;
	}
	if (!m_indicator.Initialize(m_strLastError))
	{
		m_nErrorCode = 10;
		Finalize();
		return false;
	}
	m_pHQService = new CHQService(m_pHQMarket);
	m_pStrategyService = new CStrategyService(m_pHQService, m_pTradeService, m_pAccountService, m_indicator);
	if (!m_pStrategyService->Initialize())
	{
		m_nErrorCode = 11;
		m_strLastError = m_pStrategyService->GetLastError();
		Finalize();
		return false;
	}
	m_pEngineCenter = new CEngineCenter(m_pTcpServer, m_pHQMarket, m_pBroker, m_pHQService, m_pStrategyService, m_pTradeService, m_pAccountService);
	if (!m_pEngineCenter->Initialize())
	{
		m_nErrorCode = 12;
		m_strLastError = "Engine center initialization failed";
		Finalize();
		return false;
	}
	m_pAccountService->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, m_pTradeService));
	if (!m_pBroker->Start())
	{
		m_nErrorCode = 13;
		m_strLastError = "Broker session initialization failed";
		Finalize();
		return false;
	}
	m_bInitialized = true;
	return true;
}

bool CBootLoader::Run()
{
	if (!m_bInitialized || (nullptr == m_pTcpServer) || (nullptr == m_pHttpServer))
	{
		m_nErrorCode = 14;
		m_strLastError = "Boot loader is not initialized";
		return false;
	}
	if (0 != m_pTcpServer->Initialize())
	{
		m_nErrorCode = 15;
		m_strLastError = "TCP server initialization failed";
		return false;
	}
	if (0 != m_pHttpServer->Initialize())
	{
		m_nErrorCode = 16;
		m_strLastError = "HTTP server initialization failed";
		return false;
	}
	if (!m_pHQMarket->Start())
	{
		m_nErrorCode = 17;
		m_strLastError = "HQMarket session configuration is invalid";
		return false;
	}
	std::jthread tcpServerThread(std::bind_front(&net::CTcpServer::Start, m_pTcpServer, true));
	m_pHttpServer->Start(true);
	m_pTcpServer->ShutDown();
	return true;
}

void CBootLoader::Finalize()
{
	if (nullptr != m_pHttpServer)
	{
		m_pHttpServer->ShutDown();
	}
	if (nullptr != m_pTcpServer)
	{
		m_pTcpServer->ShutDown();
	}
	// Quiesce request callbacks before stopping the services they borrow.
	if (nullptr != m_pTcpServer)
	{
		m_pTcpServer->Drain();
		m_pTcpServer->ClearHandlers();
	}
	if (nullptr != m_pStrategyService)
	{
		m_pStrategyService->StopAll();
	}
	if (nullptr != m_pHQMarket)
	{
		m_pHQMarket->Stop();
	}
	if (nullptr != m_pBroker)
	{
		m_pBroker->Stop();
	}
	if (nullptr != m_pTradeService)
	{
		m_pTradeService->Stop();
	}
	if (nullptr != m_pAccountService)
	{
		m_pAccountService->Stop();
	}
	if (nullptr != m_pBroker)
	{
		m_pBroker->ClearHandlers();
	}
	if (nullptr != m_pHQMarket)
	{
		m_pHQMarket->ClearHandlers();
	}
	// All callback producers have stopped; borrowers can now be destroyed.
	delete m_pEngineCenter;
	m_pEngineCenter = nullptr;
	delete m_pStrategyService;
	m_pStrategyService = nullptr;
	m_indicator.Finalize();
	delete m_pHQService;
	m_pHQService = nullptr;
	delete m_pTradeService;
	m_pTradeService = nullptr;
	delete m_pAccountService;
	m_pAccountService = nullptr;
	delete m_pBroker;
	m_pBroker = nullptr;
	delete m_pHQMarket;
	m_pHQMarket = nullptr;
	delete m_pHttpServer;
	m_pHttpServer = nullptr;
	delete m_pTcpServer;
	m_pTcpServer = nullptr;
	CDBEngine::InstanceRef().Close();
	m_bInitialized = false;
}

net::CTcpServer& CBootLoader::GetTcpServer()
{
	return *m_pTcpServer;
}

net::CHttpServer& CBootLoader::GetHttpServer()
{
	return *m_pHttpServer;
}

const std::string& CBootLoader::GetLastError() const
{
	return m_strLastError;
}

int CBootLoader::GetErrorCode() const
{
	return m_nErrorCode;
}
