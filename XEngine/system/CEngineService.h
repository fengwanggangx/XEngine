#ifndef XENGINE_SYSTEM_CENGINESERVICE_H
#define XENGINE_SYSTEM_CENGINESERVICE_H

#include "../network/common_net.h"
#include "../request/MarketTypes.h"
#include "../request/request.h"
#include "CSubscriptionMgr.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CSession;
class CStrategyService;
class CTradeService;
enum class SessionState;

namespace net
{
	class CTcpServer;
}

class CEngineService final
{
	friend class CServiceRoutingTests;

private:
	struct PendingSubscription
	{
		net::_TyConnectionId m_router_id{ -1 };
		_TyRequestId m_requestId{ 0 };
		CQuoteInfo m_quote;
		std::chrono::steady_clock::time_point m_deadline;
	};

	struct CPendingQueryClient
	{
		net::_TyConnectionId m_router_id{ -1 };
		_TyRequestId m_requestId{ 0 };
	};

	struct CPendingSubscriptionBatch
	{
		CPendingQueryClient m_client;
		_TyRequest m_type{ _TyRequest::UNKNOWN };
		std::chrono::steady_clock::time_point m_deadline;
		std::vector<CQuoteInfo> m_requested;
		std::vector<CQuoteInfo> m_changed;
	};
	struct CPendingQuery
	{
		_TyRequestId m_router_id{ 0 };
		std::string m_strKey;
		_TyRequest m_type{ _TyRequest::UNKNOWN };
		std::chrono::steady_clock::time_point m_deadline;
		std::vector<CPendingQueryClient> m_clients;
	};

public:
	CEngineService(net::CTcpServer* pTcpServer, CSession* pSession, CStrategyService* pStrategyService, CTradeService* pTradeService);
	~CEngineService();
	CEngineService(const CEngineService& arg) = delete;
	CEngineService& operator=(const CEngineService& arg) = delete;

	bool Initialize();

private:
	int OnNetEvent(const net::CNetEvent& ev);
	void BroadcastTrading(const CRequest& push);
	void OnClientRequest(net::_TyConnectionId id, const CRequest& req);
	bool HandleAuth(net::_TyConnectionId id, const CRequest& req);
	bool HandleRegisterAuth(net::_TyConnectionId id, const CRequest& req);
	bool HandleSubscription(net::_TyConnectionId id, const CRequest& req);
	bool HandleMarketQuery(net::_TyConnectionId id, const CRequest& req);
	bool HandleHeartbeat(net::_TyConnectionId id, const CRequest& req);
	bool HandleStrategyRequest(net::_TyConnectionId id, const CRequest& req);
	bool HandleTradeRequest(net::_TyConnectionId id, const CRequest& req);
	bool HandleTradeAccountRequest(net::_TyConnectionId id, const CRequest& req);

	int HandleDisconnected(net::_TyConnectionId id);
	bool HandleReAuth(const CRequest& req);
	void OnHQMarketResponse(const CRequest& req);
	void OnHQMarketState(SessionState state, const std::string& strMessage);
	void ExpirePendingSubscriptions();
	void FailPendingSubscriptions(const std::string& strReason);
	void ExpirePendingQueries();
	void FailPendingQueries(const std::string& strReason);
	bool RouteQueryRequest(const CRequest& response);

private:
	bool IsAuthenticated(net::_TyConnectionId id) const;
	CQuoteInfo GetQuoteInfo(const CRequest& req) const;
	void SendSubscriptionResponse(net::_TyConnectionId id, _TyRequestId requestId, bool bAccepted, const std::string& strReason, _TyRequest type = _TyRequest::MARKET_SUBSCRIBE, const std::vector<CQuoteInfo>& subscriptions = {}) const;
	std::string GetMarketResponseKey(const CRequest& req) const;
	std::string GetQueryKey(const CRequest& req) const;
	void SendQueryError(const CPendingQueryClient& client, _TyRequest type, int nErrorCode, const std::string& strMessage) const;
	void SendResponse(const CRequest& req, int nErrorCode, const std::string& strMessage) const;
	bool Login(const CRequest& req, std::string& strToken);

private:
	std::unordered_map<_TyRequest, std::function<bool(net::_TyConnectionId, const CRequest&)>> m_handler;

	mutable std::mutex m_mtx_state;
	std::unordered_set<net::_TyConnectionId> m_auth_clients;
	std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_client_tokens;

	std::unordered_map<net::_TyConnectionId, std::string> m_clientUsers;
	std::unordered_map<std::string, std::string> m_tokenUsers;
	CSubscriptionMgr m_subscriptions;
	std::unordered_map<std::string, std::vector<PendingSubscription>> m_pending_subscriptions;
	std::unordered_map<_TyRequestId, CPendingSubscriptionBatch> m_pending_batch_subscriptions;
	std::unordered_map<std::string, CPendingQuery> m_pending_queries;
	std::atomic_uint64_t m_router_id{ std::uint64_t(1) << 63 };

private:
	// Borrowed from CBootLoader; callback producers stop before service destruction.
	net::CTcpServer* m_pTcpServer{ nullptr };
	CSession* m_pSession{ nullptr };
	CStrategyService* m_pStrategyService{ nullptr };
	CTradeService* m_pTradeService{ nullptr };
};

#endif
