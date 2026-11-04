#ifndef XENGINE_SYSTEM_CENGINECENTER_H
#define XENGINE_SYSTEM_CENGINECENTER_H

#include "../network/common_net.h"
#include "../request/request.h"

#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>

class CSession;
class CHQService;

enum class SessionSource
{
	HQMarket,
	Broker
};
class CStrategyService;
class CTradeService;
class CAccountService;
enum class SessionState;

namespace net
{
	class CTcpServer;
}

class CEngineCenter final
{
	friend class CServiceRoutingTests;

public:
	~CEngineCenter();
	CEngineCenter(const CEngineCenter& arg) = delete;
	CEngineCenter& operator=(const CEngineCenter& arg) = delete;
	CEngineCenter(net::CTcpServer* pTcpServer, CSession* pHQSession, CSession* pBrokerSession, CHQService* pHQService, CStrategyService* pStrategyService, CTradeService* pTradeService, CAccountService* pAccountService);

	bool Initialize();
	void OnRequest(const CRequest& req);
	bool OnReply(SessionSource source, const CRequest& req);
	void OnSessionState(SessionSource source, SessionState state, const std::string& strReason);

private:
	int OnNetEvent(const net::CNetEvent& ev);
	bool BroadcastTrading(const CRequest& push);

	bool HandleAuth(net::_TyConnectionId id, const CRequest& req);
	bool HandleHeartbeat(net::_TyConnectionId id, const CRequest& req);
	bool HandleRegisterAuth(net::_TyConnectionId id, const CRequest& req);
	bool HandleTradeRequest(net::_TyConnectionId id, const CRequest& req);
	bool HandleStrategyRequest(net::_TyConnectionId id, const CRequest& req);
	bool HandleTradeAccountRequest(net::_TyConnectionId id, const CRequest& req, const std::string& strUser);

	bool HandleReAuth(const CRequest& req);
	int HandleDisconnected(net::_TyConnectionId id);

private:
	bool Login(const CRequest& req, std::string& strToken);
	std::pair<bool, std::string> IsAuthenticated(net::_TyConnectionId id) const;
	void SendResponse(const CRequest& req, int nErrorCode, const std::string& strMessage) const;

private:
	mutable std::mutex m_mtx_state;
	std::unordered_map<net::_TyConnectionId, std::string> m_authClients;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_clientTokens;

	std::unordered_map<std::string, std::string> m_tokenUsers;

private:
	// Borrowed from CBootLoader; callback producers stop before service destruction.
	net::CTcpServer* m_pTcpServer{ nullptr };
	CSession* m_pHQSession{ nullptr };
	CSession* m_pBrokerSession{ nullptr };
	CHQService* m_pHQService{ nullptr };
	CStrategyService* m_pStrategyService{ nullptr };
	CTradeService* m_pTradeService{ nullptr };
	CAccountService* m_pAccountService{ nullptr };
};

#endif
