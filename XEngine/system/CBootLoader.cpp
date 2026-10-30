#include "CBootLoader.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../ini/CINIHandler.h"
#include "../network/CHttpServer.h"
#include "../network/CTcpServer.h"
#include "CHostMgr.h"
#include "CSession.h"
#include "CEngineService.h"
#include "../strategy/CTradeService.h"
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

	m_pHostMgr = std::make_unique<CHostMgr>();
	if (!m_pHostMgr->GetActiveHost().has_value())
	{
		m_nErrorCode = 2;
		m_strLastError = "Failed to initialize CHostMgr";
		return false;
	}

	ini::CINIHandler& hIni = ini::CINIHandler::InstanceRef();

	m_strAccount = hIni.GetValue(ini::Config::System, "HQMarket", "account", std::string());
	if (m_strAccount.empty())
	{
		m_nErrorCode = 3;
		m_strLastError = "HQMarket account is required in ini/system.ini";
		return false;
	}

	m_strPassword = hIni.GetValue(ini::Config::System, "HQMarket", "password", std::string());
	if (m_strPassword.empty())
	{
		m_nErrorCode = 4;
		m_strLastError = "HQMarket password is required in ini/system.ini";
		return false;
	}

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

	m_pTcpServer = std::make_unique<net::CTcpServer>(nTcpPort);
	m_pHttpServer = std::make_unique<net::CHttpServer>(nHttpPort);

	std::optional<CHostInfo> host = m_pHostMgr->GetActiveHost();
	m_pSession = std::make_unique<CSession>(CLoginInfo{ m_strAccount, m_strPassword, {}, host.value() });
	m_pTradeService = std::make_unique<CTradeService>();
	if (!m_pTradeService->Initialize())
	{
		m_nErrorCode = 9;
		m_strLastError = m_pTradeService->GetLastError();
		return false;
	}
	m_pStrategyService = std::make_unique<CStrategyService>(m_pSession.get(), m_pTradeService.get());
	if (!m_pStrategyService->Initialize())
	{
		m_nErrorCode = 10;
		m_strLastError = m_pStrategyService->GetLastError();
		return false;
	}
	m_pEngineService = std::make_unique<CEngineService>(m_pTcpServer.get(), m_pSession.get(), m_pStrategyService.get(), m_pTradeService.get());
	if (!m_pEngineService->Initialize())
	{
		m_nErrorCode = 11;
		m_strLastError = "Engine service initialization failed";
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
	if (!m_pSession->Start())
	{
		m_nErrorCode = 12;
		m_strLastError = "HQMarket session configuration is invalid";
		return false;
	}
	std::jthread tcpServerThread(std::bind_front(&net::CTcpServer::Start, m_pTcpServer.get(), true));
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
	if (nullptr != m_pSession)
	{
		m_pSession->Stop();
	}
	if (nullptr != m_pTradeService)
	{
		m_pTradeService->Stop();
	}
	// All callback producers have stopped; borrowers can now be destroyed.
	m_pEngineService.reset();
	m_pStrategyService.reset();
	m_pTradeService.reset();
	m_pSession.reset();
	m_pHttpServer.reset();
	m_pTcpServer.reset();
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

const std::string& CBootLoader::GetAccount() const
{
	return m_strAccount;
}

const std::string& CBootLoader::GetPassword() const
{
	return m_strPassword;
}

CHostMgr& CBootLoader::GetHostMgr()
{
	return *m_pHostMgr;
}

const std::string& CBootLoader::GetLastError() const
{
	return m_strLastError;
}

int CBootLoader::GetErrorCode() const
{
	return m_nErrorCode;
}
