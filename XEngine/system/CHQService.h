#pragma once
#include "../network/common_net.h"
#include "../request/MarketTypes.h"
#include "../request/request.h"
#include "CSubscriptionMgr.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

class CSession;
enum class SessionState;

class CHQService final
{
	friend class CServiceRoutingTests;

private:
	struct CPendingSubscription
	{
		net::_TyConnectionId m_routerId{ -1 };
		_TyRequestId m_requestId{ 0 };
		CQuoteInfo m_quote;
		std::chrono::steady_clock::time_point m_deadline;
	};

	struct CPendingQueryClient
	{
		net::_TyConnectionId m_routerId{ -1 };
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
		_TyRequestId m_routerId{ 0 };
		std::string m_strKey;
		_TyRequest m_type{ _TyRequest::UNKNOWN };
		std::chrono::steady_clock::time_point m_deadline;
		std::vector<CPendingQueryClient> m_clients;
	};

public:
	explicit CHQService(CSession* pSession);
	CHQService(const CHQService& arg) = delete;
	CHQService& operator=(const CHQService& arg) = delete;

	bool OnReply(const CRequest& req);
	bool OnRequest(const CRequest& req);
	int HandleClientDisconnected(net::_TyConnectionId id);
	void OnSessionState(SessionState state, const std::string& strMessage);

private:
	void ExpirePendingQueries();
	void ExpirePendingSubscriptions();
	bool RouteQueryRequest(const CRequest& response);
	CQuoteInfo GetQuoteInfo(const CRequest& req) const;
	std::string GetQueryKey(const CRequest& req) const;
	void FailPendingQueries(const std::string& strReason);
	std::string GetMarketResponseKey(const CRequest& req) const;
	void FailPendingSubscriptions(const std::string& strReason);
	bool HandleSubscription(net::_TyConnectionId id, const CRequest& req);
	bool HandleQueryHQRequest(net::_TyConnectionId id, const CRequest& req);
	void SendQueryError(const CPendingQueryClient& client, _TyRequest type, int nErrorCode, const std::string& strMessage) const;
	void SendSubscriptionResponse(net::_TyConnectionId id, _TyRequestId requestId, bool bAccepted, const std::string& strReason, _TyRequest type = _TyRequest::MARKET_SUBSCRIBE, const std::vector<CQuoteInfo>& subscriptions = {}) const;
	std::mutex m_mtx_state;
	CSubscriptionMgr m_subscriptions;
	std::unordered_map<std::string, std::vector<CPendingSubscription>> m_pendingSubscriptions;
	std::unordered_map<_TyRequestId, CPendingSubscriptionBatch> m_pendingBatchSubscriptions;
	std::unordered_map<std::string, CPendingQuery> m_pendingQueries;
	std::atomic_uint64_t m_routerId{ std::uint64_t(1) << 63 };

	CSession* m_pSession{ nullptr };
};
