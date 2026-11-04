#include "CBootLoader.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../ini/CINIHandler.h"
#include "../network/CHttpServer.h"
#include "../network/CTcpServer.h"
#include "CSession.h"
#include "CEngineService.h"
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

	if (nullptr != m_pTcpServer || nullptr != m_pHttpServer || nullptr != m_pHQMarket || nullptr != m_pBroker || nullptr != m_pAccountService || nullptr != m_pTradeService || nullptr != m_pStrategyService || nullptr != m_pEngineService)
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

	std::string strMySqlHost = hIni.GetValue(ini::Config::System, "MySQL", "host", "");
	int nMySqlPort = hIni.GetValue(ini::Config::System, "MySQL", "port", -1);

	std::string strMySqlAccount = hIni.GetValue(ini::Config::System, "MySQL", "account", "");
	std::string strMySqlPassword = hIni.GetValue(ini::Config::System, "MySQL", "password", "");
	std::string strMySqlDatabase = hIni.GetValue(ini::Config::System, "MySQL", "database", "");

	int nMySqlPoolSize = hIni.GetValue(ini::Config::System, "MySQL", "pool_size", -1);

	if (strMySqlHost.empty() || (nMySqlPort <= 0) || strMySqlAccount.empty() || strMySqlPassword.empty() || strMySqlDatabase.empty() || (nMySqlPoolSize <= 0))
	{
		m_nErrorCode = 5;
		m_strLastError = "HQMarket mysql param error";
		return false;
	}

	db::CConnectParam dbParam(strMySqlHost, static_cast<unsigned int>(nMySqlPort), strMySqlAccount, strMySqlPassword, strMySqlDatabase, "utf8mb4");
	if (0 != CDBEngine::InstanceRef().Initialize(db::em_database::mysql, dbParam, nMySqlPoolSize))
	{
		m_nErrorCode = 6;
		m_strLastError = "MySQL initialization failed";
		return false;
	}
	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if ((nullptr == db) || (0 != db->ExecSqlFile(m_exec / "sql" / "table_create.sql")))
	{
		m_nErrorCode = 7;
		m_strLastError = "Failed to initialize MySQL tables";
		return false;
	}

	int nTcpPort = hIni.GetValue(ini::Config::System, "System", "tcp_port", -1);
	int nHttpPort = hIni.GetValue(ini::Config::System, "System", "http_port", -1);
	if ((nTcpPort <= 0) || (nHttpPort <= 0))
	{
		m_nErrorCode = 8;
		m_strLastError = "HQMarket tcp_port && http_port is required in ini/system.ini";
		return false;
	}

	m_pTcpServer = new net::CTcpServer(nTcpPort);
	m_pHttpServer = new net::CHttpServer(nHttpPort);

	std::string strhqMarketHost = "HQMarket";
	CLoginInfo hq;
	hq.m_strUser = hIni.GetValue(ini::Config::System, strhqMarketHost, "user", std::string());
	hq.m_strPassword = hIni.GetValue(ini::Config::System, strhqMarketHost, "password", std::string());
	hq.m_host.m_strName = strhqMarketHost;
	hq.m_host.m_strHost = hIni.GetValue(ini::Config::System, strhqMarketHost, "host", std::string("127.0.0.1"));
	hq.m_host.m_nPort = hIni.GetValue(ini::Config::System, strhqMarketHost, "port", 9803);
	m_pHQMarket = new CSession(hq);

	std::string strBrokerHost = "Broker";
	CLoginInfo broker;
	broker.m_strUser = hIni.GetValue(ini::Config::System, strBrokerHost, "user", std::string());
	broker.m_strPassword = hIni.GetValue(ini::Config::System, strBrokerHost, "password", std::string());
	broker.m_host.m_strName = strBrokerHost;
	broker.m_host.m_strHost = hIni.GetValue(ini::Config::System, strBrokerHost, "host", std::string("127.0.0.1"));
	broker.m_host.m_nPort = hIni.GetValue(ini::Config::System, strBrokerHost, "port", 9804);
	broker.m_bOrdered = true;
	m_pBroker = new CSession(broker);

	m_pAccountService = new CAccountService(m_pBroker);
	if (!m_pAccountService->Initialize())
	{
		m_nErrorCode = 13;
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
	m_pStrategyService = new CStrategyService(m_pHQMarket, m_pTradeService, m_pAccountService);
	if (!m_pStrategyService->Initialize())
	{
		m_nErrorCode = 10;
		m_strLastError = m_pStrategyService->GetLastError();
		return false;
	}
	m_pEngineService = new CEngineService(m_pTcpServer, m_pHQMarket, m_pStrategyService, m_pTradeService, m_pAccountService);
	if (!m_pEngineService->Initialize())
	{
		m_nErrorCode = 11;
		m_strLastError = "Engine service initialization failed";
		return false;
	}
	m_pAccountService->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, m_pTradeService));
	if (broker.Valid() && !m_pBroker->Start())
	{
		m_nErrorCode = 14;
		m_strLastError = "Broker session initialization failed";
		return false;
	}
	m_bInitialized = true;
	return true;
}

bool CBootLoader::Run()
{
	if (!m_bInitialized || (nullptr == m_pTcpServer) || (nullptr == m_pHttpServer))
	{
		m_nErrorCode = 4;
		m_strLastError = "Boot loader is not initialized";
		return false;
	}
	if (0 != m_pTcpServer->Initialize())
	{
		m_nErrorCode = 4;
		m_strLastError = "TCP server initialization failed";
		return false;
	}
	if (0 != m_pHttpServer->Initialize())
	{
		m_nErrorCode = 5;
		m_strLastError = "HTTP server initialization failed";
		return false;
	}
	if (!m_pHQMarket->Start())
	{
		m_nErrorCode = 12;
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
	// All callback producers have stopped; borrowers can now be destroyed.
	delete m_pEngineService;
	m_pEngineService = nullptr;
	delete m_pStrategyService;
	m_pStrategyService = nullptr;
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
