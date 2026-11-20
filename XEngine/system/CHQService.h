#pragma once
#include "../network/common_net.h"
#include "../request/MarketTypes.h"
#include "../request/request.h"
#include "CSubscriptionMgr.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class CSession;
enum class SessionState;

struct CPendingQueryClient
{
	net::_TyConnectionId m_router_id{ -1 };
	_TyRequestId m_request_id{ 0 };
	std::function<void(const CRequest&)> m_handler;
};

struct CPendingQuery
{
	_TyRequestId m_router_id{ 0 };
	std::string m_key;
	_TyRequest m_type{ _TyRequest::UNKNOWN };
	std::chrono::steady_clock::time_point m_deadline;
	std::vector<CPendingQueryClient> m_clients;
};

struct CPendingSubscriptionBatch
{
	CPendingQueryClient m_client;
	_TyRequest m_type{ _TyRequest::UNKNOWN };
	std::chrono::steady_clock::time_point m_deadline;
	std::vector<CQuoteInfo> m_requested;
	std::vector<CQuoteInfo> m_changed;
	std::string m_error_message;
};

struct CUpstreamSubscription
{
	CQuoteInfo m_quote;
	_TyRequestId m_request_id{ 0 };
	_TyRequest m_type{ _TyRequest::UNKNOWN };
	bool m_confirmed{ false };
	std::chrono::steady_clock::time_point m_deadline;
	bool m_uncertain{ false };
};

class CHQService final
{
	friend class CServiceRoutingTests;
	friend class CMarketSubscriptionTests;

public:
	explicit CHQService(CSession* pSession);
	CHQService(const CHQService& arg) = delete;
	CHQService& operator=(const CHQService& arg) = delete;

	void Release(HQOrigin type, std::uint64_t id);
	void Subscribe(HQOrigin type, std::uint64_t id, const std::vector<CQuoteInfo>& quotes);

	using _TyQueryHandler = std::function<void(const CRequest&)>;
	bool QueryBars(const CQuoteInfo& quote, std::int64_t begin, std::int64_t end, _TyQueryHandler&& handler);
	bool OnReply(const CRequest& req);
	bool OnRequest(const CRequest& req);
	int HandleClientDisconnected(net::_TyConnectionId id);
	void OnSessionState(SessionState state, const std::string& strMessage);

private:
	// Subscription helpers require m_subscriptions_mutex; sends are buffered and serialized
	// with demand changes so a late unsubscribe cannot overtake a new subscriber.
	bool HasDemand(const CQuoteInfo& quote) const;
	void CompleteSubscriptions();
	void ReconcileSubscription(const CQuoteInfo& quote, bool bRefreshSnapshot = false);
	void ReconcileSubscriptions(const std::vector<CQuoteInfo>& quotes, bool bRefreshSnapshot = false);
	bool HandleSubscriptionReply(const CRequest& req);
	void FinishSubscription(const std::string& strKey, bool bAccepted, const std::string& strReason, bool bUncertain = false);
	void ExpirePendingQueries();
	void ExpirePendingSubscriptions();
	bool RouteQueryRequest(const CRequest& response);
	CQuoteInfo GetQuoteInfo(const CRequest& req) const;
	std::string GetQueryKey(const CRequest& req) const;
	void FailPendingQueries(const std::string& strReason);
	std::string GetMarketResponseKey(const CRequest& req) const;
	void FailPendingSubscriptions(const std::string& strReason);
	bool OnSubscriptionRequest(net::_TyConnectionId id, const CRequest& req);
	bool HandleQueryHQRequest(net::_TyConnectionId id, const CRequest& req, _TyQueryHandler handler = {});
	void SendQueryError(const CPendingQueryClient& client, _TyRequest type, int nErrorCode, const std::string& strMessage) const;
	void SendSubscriptionResponse(net::_TyConnectionId id, _TyRequestId requestId, bool bAccepted, const std::string& strReason, _TyRequest type = _TyRequest::MARKET_SUBSCRIBE, const std::vector<CQuoteInfo>& subscriptions = {}) const;

private:
	std::mutex m_state_mutex;
	CSubscriptionMgr m_subscriptions;
	std::mutex m_subscriptions_mutex;
	bool m_market_ready{ false };
	std::unordered_map<std::string, CUpstreamSubscription> m_upstream_subscriptions;
	std::unordered_map<_TyRequestId, CPendingSubscriptionBatch> m_pending_batch_subscriptions;
	std::unordered_map<std::string, CPendingQuery> m_pending_queries;
	std::atomic_uint64_t m_router_id{ std::uint64_t(1) << 63 };

	CSession* m_session{ nullptr };
};
