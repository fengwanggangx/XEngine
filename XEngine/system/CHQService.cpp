#include "CHQService.h"

#include "../network/CNetTools.h"
#include "../network/common_net.h"
#include "../request/RequestCenter.h"
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../request/v1/market.pb.h"

#include "CSession.h"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	constexpr int InvalidRequest = 1001;
	constexpr int InvalidSubscription = 1006;
	constexpr int UpstreamUnavailable = 1007;
	constexpr int UpstreamTimeout = 1008;
	constexpr int TooManyPendingQueries = 1009;
	constexpr std::size_t MaxPendingQueries = 256;
	constexpr std::chrono::seconds UpstreamRequestTimeout{ 10 };
} // namespace

CHQService::CHQService(CSession* pSession) : m_pSession(pSession)
{
}

bool CHQService::OnRequest(const CRequest& req)
{
	ExpirePendingQueries();
	ExpirePendingSubscriptions();
	if ((_TyRequest::MARKET_SUBSCRIBE == req.GetType()) || (_TyRequest::MARKET_UNSUBSCRIBE == req.GetType()))
	{
		return HandleSubscription(req.GetConnectionId(), req);
	}
	return HandleQueryHQRequest(req.GetConnectionId(), req);
}

CQuoteInfo CHQService::GetQuoteInfo(const CRequest& req) const
{
	std::string strSecurity = req.GetExtraData("security");
	std::size_t nDot = strSecurity.rfind('.');
	if ((std::string::npos == nDot) || (0 == nDot) || (strSecurity.size() - 1 == nDot))
	{
		return {};
	}
	return CQuoteInfo(strSecurity.substr(0, nDot), ParseMarket(strSecurity.substr(nDot + 1)), ParseChannel(req.GetExtraData("channel")));
}

void CHQService::SendSubscriptionResponse(net::_TyConnectionId id, _TyRequestId requestId, bool bAccepted, const std::string& strReason, _TyRequest type, const std::vector<CQuoteInfo>& subscriptions) const
{
	CRequest response;
	response.SetId(requestId);
	response.SetType(type);
	response.SetErrorInfo(bAccepted ? 0 : InvalidSubscription, bAccepted ? "" : strReason);
	_TySubscriptionAck ack;
	for (const auto& quote : subscriptions)
	{
		hqmarket::market::v1::SubscriptionResult* pResult = ack.add_results();
		pResult->mutable_security()->set_symbol(quote.m_security.m_strCode);
		pResult->mutable_security()->set_exchange(static_cast<_TyMarketExchange>(quote.m_security.m_market));
		pResult->set_channel(static_cast<hqmarket::market::v1::Channel>(quote.m_channel));
		pResult->set_accepted(bAccepted);
		pResult->set_reason(bAccepted ? "" : strReason);
	}
	response.SetData(ack);
	net::SendRequest(id, response);
}

std::string CHQService::GetMarketResponseKey(const CRequest& req) const
{
	const _TyReqData& message = req.GetData();
	_TySecurity security;
	hqmarket::market::v1::Channel channel = hqmarket::market::v1::CHANNEL_UNSPECIFIED;
	if (message.has_quote())
	{
		security = message.quote().security();
		channel = hqmarket::market::v1::CHANNEL_QUOTE;
	}
	else if (message.has_depth())
	{
		security = message.depth().security();
		channel = hqmarket::market::v1::CHANNEL_DEPTH;
	}
	if (hqmarket::market::v1::CHANNEL_UNSPECIFIED != channel)
	{
		Exchange exchange = static_cast<Exchange>(static_cast<int>(security.exchange()));
		Channel marketChannel = static_cast<Channel>(static_cast<int>(channel));
		return CQuoteInfo(security.symbol(), exchange, marketChannel).String();
	}
	if (message.has_subscription_ack() && (0 != message.subscription_ack().results_size()))
	{
		const hqmarket::market::v1::SubscriptionResult& result = message.subscription_ack().results(0);
		Exchange exchange = static_cast<Exchange>(static_cast<int>(result.security().exchange()));
		Channel channel = static_cast<Channel>(static_cast<int>(result.channel()));
		return CQuoteInfo(result.security().symbol(), exchange, channel).String();
	}
	if ((_TyRequest::MARKET_SUBSCRIBE == req.GetType()) || (_TyRequest::MARKET_UNSUBSCRIBE == req.GetType()))
	{
		CQuoteInfo quote = GetQuoteInfo(req);
		return quote.IsValid() ? quote.String() : std::string();
	}
	return {};
}

bool CHQService::OnReply(const CRequest& req)
{
	ExpirePendingSubscriptions();
	ExpirePendingQueries();
	if ((_TyRequest::MARKET_SUBSCRIBE == req.GetType()) || (_TyRequest::MARKET_UNSUBSCRIBE == req.GetType()))
	{
		CPendingSubscriptionBatch pending;
		bool bBatch = false;
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			const auto mIter = m_pendingBatchSubscriptions.find(req.GetId());
			if ((m_pendingBatchSubscriptions.end() != mIter) && (req.GetType() == mIter->second.m_type))
			{
				pending = std::move(mIter->second);
				m_pendingBatchSubscriptions.erase(mIter);
				bBatch = true;
			}
		}
		if (bBatch)
		{
			std::optional<std::pair<int, std::string>> error = req.GetErrorInfo();
			if (error.has_value())
			{
				if (_TyRequest::MARKET_SUBSCRIBE == pending.m_type)
				{
					m_subscriptions.Unsubscribe(pending.m_client.m_routerId, pending.m_changed);
				}
				else
				{
					m_subscriptions.Subscribe(pending.m_client.m_routerId, pending.m_changed);
				}
			}
			SendSubscriptionResponse(pending.m_client.m_routerId, pending.m_client.m_requestId, !error.has_value(), error.has_value() ? error->second : "", pending.m_type, pending.m_requested);
			return true;
		}
	}
	if (RouteQueryRequest(req))
	{
		return true;
	}
	std::string strKey = GetMarketResponseKey(req);
	if (strKey.empty())
	{
		return false;
	}

	_TyRequest type = req.GetType();
	if ((_TyRequest::MARKET_SUBSCRIBE == type) || (_TyRequest::MARKET_UNSUBSCRIBE == type))
	{
		std::vector<CPendingSubscription> pending;
		std::optional<std::pair<int, std::string>> error = req.GetErrorInfo();
		bool bAccepted = !error.has_value();
		std::string strReason = error.has_value() ? error->second : std::string();
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			const auto mIter = m_pendingSubscriptions.find(strKey);
			if (m_pendingSubscriptions.end() == mIter)
			{
				return false;
			}
			pending = std::move(mIter->second);
			m_pendingSubscriptions.erase(mIter);
		}
		if (!bAccepted)
		{
			m_subscriptions.RemoveSubscription(strKey);
		}
		for (const auto& subscription : pending)
		{
			SendSubscriptionResponse(subscription.m_routerId, subscription.m_requestId, bAccepted, strReason, _TyRequest::MARKET_SUBSCRIBE, { subscription.m_quote });
		}
		return true;
	}

	CRequest push = req;
	push.SetId(0);
	std::vector<net::_TyConnectionId> ids = m_subscriptions.GetSubscriberIds(strKey);
	std::vector<net::_TyConnectionId> disconnected;
	for (net::_TyConnectionId id : ids)
	{
		if (!net::SendRequest(id, push))
		{
			disconnected.emplace_back(id);
		}
	}
	for (net::_TyConnectionId id : disconnected)
	{
		HandleClientDisconnected(id);
	}
	return true;
}

void CHQService::OnSessionState(SessionState state, const std::string& strMessage)
{
	if (SessionState::Ready == state)
	{
		for (const CQuoteInfo& quote : m_subscriptions.GetSubscriptions())
		{
			m_pSession->SendRequest(request::Subscription(quote));
		}
		return;
	}
	if ((SessionState::Disconnected == state) || (SessionState::Stopping == state))
	{
		FailPendingSubscriptions(strMessage.empty() ? "HQMarket connection lost" : strMessage);
		FailPendingQueries(strMessage.empty() ? "HQMarket connection lost" : strMessage);
	}
}

std::string CHQService::GetQueryKey(const CRequest& req) const
{
	_TyRequest type = req.GetType();
	if (_TyRequest::MARKET_QUOTE_QUERY == type)
	{
		return req.GetTypeName() + ":" + req.GetExtraData("security");
	}
	if (_TyRequest::MARKET_SECURITIES_QUERY == type)
	{
		return req.GetTypeName();
	}
	if (_TyRequest::MARKET_SECTORS_QUERY == type)
	{
		std::string strSectorType = req.GetExtraData("sector_type");
		return strSectorType.empty() ? std::string() : req.GetTypeName() + ":" + strSectorType;
	}
	if (_TyRequest::MARKET_SECTOR_CONSTITUENTS_QUERY == type)
	{
		std::string strSectorType = req.GetExtraData("sector_type");
		std::string strSectorCode = req.GetExtraData("sector_code");
		return strSectorType.empty() || strSectorCode.empty() ? std::string() : req.GetTypeName() + ":" + strSectorType + ":" + strSectorCode;
	}
	if (_TyRequest::MARKET_BARS_QUERY == type)
	{
		return req.GetTypeName() + ":" + req.GetExtraData("security") + ":" + req.GetExtraData("channel") + ":" + req.GetExtraData("begin_time_ms") + ":" + req.GetExtraData("end_time_ms");
	}
	return {};
}

bool CHQService::HandleQueryHQRequest(net::_TyConnectionId id, const CRequest& req)
{
	std::string strKey = GetQueryKey(req);
	if (strKey.empty())
	{
		net::SendError(id, req, InvalidRequest, "invalid market query");
		return false;
	}

	_TyRequestId routerId = 0;
	bool bTooManyPending = false;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto mIter = m_pendingQueries.find(strKey);
		if (m_pendingQueries.end() != mIter)
		{
			mIter->second.m_clients.emplace_back(CPendingQueryClient{ id, req.GetId() });
			return true;
		}
		if (MaxPendingQueries <= m_pendingQueries.size())
		{
			bTooManyPending = true;
		}
		else
		{
			routerId = m_routerId.fetch_add(1);
			CPendingQuery pending;
			pending.m_routerId = routerId;
			pending.m_strKey = strKey;
			pending.m_type = req.GetType();
			pending.m_deadline = std::chrono::steady_clock::now() + UpstreamRequestTimeout;
			pending.m_clients.emplace_back(CPendingQueryClient{ id, req.GetId() });
			m_pendingQueries.emplace(strKey, std::move(pending));
		}
	}
	if (bTooManyPending)
	{
		net::SendError(id, req, TooManyPendingQueries, "too many pending market queries");
		return false;
	}

	CRequest router = req;
	router.SetId(routerId);
	if ((nullptr != m_pSession) && m_pSession->SendRequest(router))
	{
		return true;
	}
	CPendingQuery failed;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto mIter = m_pendingQueries.find(strKey);
		if ((m_pendingQueries.end() != mIter) && (routerId == mIter->second.m_routerId))
		{
			failed = std::move(mIter->second);
			m_pendingQueries.erase(mIter);
		}
	}
	for (const auto& client : failed.m_clients)
	{
		SendQueryError(client, failed.m_type, UpstreamUnavailable, "HQMarket is unavailable");
	}
	return false;
}

bool CHQService::RouteQueryRequest(const CRequest& req)
{
	CPendingQuery pending;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		_TyRequestId routerId = req.GetId();
		const auto mIter = std::find_if(m_pendingQueries.begin(), m_pendingQueries.end(), [routerId](const auto& item)
		{
			return routerId == item.second.m_routerId;
		});
		if ((m_pendingQueries.end() == mIter) || (req.GetType() != mIter->second.m_type))
		{
			return false;
		}
		pending = std::move(mIter->second);
		m_pendingQueries.erase(mIter);
	}
	for (const auto& client : pending.m_clients)
	{
		CRequest s = req;
		s.SetId(client.m_requestId);
		net::SendRequest(client.m_routerId, s);
	}
	return true;
}

void CHQService::SendQueryError(const CPendingQueryClient& client, _TyRequest type, int nErrorCode, const std::string& strMessage) const
{
	CRequest response;
	response.SetId(client.m_requestId);
	response.SetType(type);
	net::SetError(response, nErrorCode, strMessage);
	net::SendRequest(client.m_routerId, response);
}

void CHQService::ExpirePendingQueries()
{
	std::vector<CPendingQuery> expired;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pendingQueries.begin(); m_pendingQueries.end() != mIter;)
		{
			if (mIter->second.m_deadline <= now)
			{
				expired.emplace_back(std::move(mIter->second));
				mIter = m_pendingQueries.erase(mIter);
			}
			else
			{
				++mIter;
			}
		}
	}
	for (const auto& pending : expired)
	{
		for (const auto& client : pending.m_clients)
		{
			SendQueryError(client, pending.m_type, UpstreamTimeout, "HQMarket request timeout");
		}
	}
}

void CHQService::FailPendingQueries(const std::string& strReason)
{
	std::vector<CPendingQuery> failed;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		failed.reserve(m_pendingQueries.size());
		for (auto& item : m_pendingQueries)
		{
			failed.emplace_back(std::move(item.second));
		}
		m_pendingQueries.clear();
	}
	for (const auto& pending : failed)
	{
		for (const auto& client : pending.m_clients)
		{
			SendQueryError(client, pending.m_type, UpstreamUnavailable, strReason);
		}
	}
}

void CHQService::ExpirePendingSubscriptions()
{
	std::vector<CPendingSubscriptionBatch> expiredBatches;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pendingBatchSubscriptions.begin(); m_pendingBatchSubscriptions.end() != mIter;)
		{
			if (mIter->second.m_deadline <= std::chrono::steady_clock::now())
			{
				expiredBatches.emplace_back(std::move(mIter->second));
				mIter = m_pendingBatchSubscriptions.erase(mIter);
			}
			else
			{
				++mIter;
			}
		}
	}
	for (const auto& pending : expiredBatches)
	{
		if (_TyRequest::MARKET_SUBSCRIBE == pending.m_type)
		{
			m_subscriptions.Unsubscribe(pending.m_client.m_routerId, pending.m_changed);
		}
		else
		{
			m_subscriptions.Subscribe(pending.m_client.m_routerId, pending.m_changed);
		}
		SendSubscriptionResponse(pending.m_client.m_routerId, pending.m_client.m_requestId, false, "HQMarket subscription timeout", pending.m_type, pending.m_requested);
	}
	std::vector<CPendingSubscription> expired;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pendingSubscriptions.begin(); m_pendingSubscriptions.end() != mIter;)
		{
			std::vector<CPendingSubscription>& values = mIter->second;
			for (auto vIter = values.begin(); values.end() != vIter;)
			{
				if (vIter->m_deadline <= now)
				{
					expired.emplace_back(*vIter);
					vIter = values.erase(vIter);
				}
				else
				{
					++vIter;
				}
			}
			mIter = values.empty() ? m_pendingSubscriptions.erase(mIter) : std::next(mIter);
		}
	}
	for (const auto& pending : expired)
	{
		m_subscriptions.Unsubscribe(pending.m_routerId, { pending.m_quote });
		if (nullptr != m_pSession)
		{
			m_pSession->SendRequest(request::UnSubscription(pending.m_quote));
		}
		SendSubscriptionResponse(pending.m_routerId, pending.m_requestId, false, "HQMarket request timeout");
	}
}

void CHQService::FailPendingSubscriptions(const std::string& strReason)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto& item : m_pendingBatchSubscriptions)
		{
			item.second.m_deadline = std::chrono::steady_clock::time_point::min();
		}
	}
	ExpirePendingSubscriptions();
	std::vector<CPendingSubscription> failed;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (const auto& item : m_pendingSubscriptions)
		{
			failed.insert(failed.end(), item.second.begin(), item.second.end());
		}
		m_pendingSubscriptions.clear();
	}
	for (const auto& pending : failed)
	{
		m_subscriptions.Unsubscribe(pending.m_routerId, { pending.m_quote });
		if (nullptr != m_pSession)
		{
			m_pSession->SendRequest(request::UnSubscription(pending.m_quote));
		}
		SendSubscriptionResponse(pending.m_routerId, pending.m_requestId, false, strReason);
	}
}

bool CHQService::HandleSubscription(net::_TyConnectionId, const CRequest& req)
{
	net::_TyConnectionId id = static_cast<net::_TyConnectionId>(req.GetConnectionId());
	bool bSubscribe = request::IsSubscriptionRequest(req);
	bool bUnsubscribe = request::IsUnSubscriptionRequest(req);

	if (!bSubscribe && !bUnsubscribe)
	{
		net::SendError(id, req, InvalidSubscription, "unsupported request");
		return false;
	}
	if (!req.GetExtraData("subscriptions").empty())
	{
		CRequest::_TyExtraRows rows;
		if (!req.GetExtraData("subscriptions", rows) || rows.empty())
		{
			net::SendError(id, req, InvalidSubscription, "subscriptions must be a nonempty JSON array");
			return false;
		}
		std::vector<CQuoteInfo> subscriptions;
		subscriptions.reserve(rows.size());
		for (const auto& row : rows)
		{
			const auto security = row.find("security");
			const auto channel = row.find("channel");
			if ((row.end() == security) || (row.end() == channel))
			{
				net::SendError(id, req, InvalidSubscription, "subscription row requires security and channel");
				return false;
			}
			CRequest item;
			item.SetExtraData("security", security->second);
			item.SetExtraData("channel", channel->second);
			CQuoteInfo quote = GetQuoteInfo(item);
			if (!quote.IsValid())
			{
				net::SendError(id, req, InvalidSubscription, "invalid subscription row");
				return false;
			}
			subscriptions.emplace_back(std::move(quote));
		}
		if (subscriptions.empty())
		{
			net::SendError(id, req, InvalidSubscription, "empty subscription batch");
			return false;
		}
		std::vector<CQuoteInfo> applied;
		applied.reserve(subscriptions.size());
		for (const auto& quote : subscriptions)
		{
			if (bSubscribe != m_subscriptions.IsSubscribed(id, quote))
			{
				applied.emplace_back(quote);
			}
		}
		std::vector<CQuoteInfo> changed;
		if (bSubscribe)
		{
			changed = m_subscriptions.Subscribe(id, subscriptions);
		}
		else
		{
			changed = m_subscriptions.Unsubscribe(id, subscriptions);
		}
		if (changed.empty())
		{
			SendSubscriptionResponse(id, req.GetId(), true, bSubscribe ? "already subscribed" : "not subscribed", req.GetType(), subscriptions);
			return true;
		}
		CRequest router = req;
		_TyRequestId routerId = m_routerId.fetch_add(1);
		router.SetId(routerId);
		CRequest::_TyExtraRows changedRows;
		changedRows.reserve(changed.size());
		for (const auto& quote : changed)
		{
			changedRows.emplace_back(std::unordered_map<std::string, std::string>{ { "security", quote.m_security.String() }, { "channel", GetChannelString(quote.m_channel) } });
		}
		router.SetExtraData("subscriptions", changedRows);
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			m_pendingBatchSubscriptions.emplace(routerId, CPendingSubscriptionBatch{ { id, req.GetId() }, req.GetType(), std::chrono::steady_clock::now() + UpstreamRequestTimeout, subscriptions, applied });
		}
		if ((nullptr != m_pSession) && m_pSession->SendRequest(router))
		{
			return true;
		}
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			m_pendingBatchSubscriptions.erase(routerId);
		}
		if (bSubscribe)
		{
			m_subscriptions.Unsubscribe(id, applied);
		}
		else
		{
			m_subscriptions.Subscribe(id, applied);
		}
		SendSubscriptionResponse(id, req.GetId(), false, "HQMarket is unavailable", req.GetType(), subscriptions);
		return false;
	}
	CQuoteInfo quote = GetQuoteInfo(req);
	if (!quote.IsValid())
	{
		net::SendError(id, req, InvalidSubscription, "invalid security or unsupported subscription channel");
		return false;
	}
	std::string strKey = quote.String();

	if (bSubscribe)
	{
		if (m_subscriptions.IsSubscribed(id, quote))
		{
			SendSubscriptionResponse(id, req.GetId(), true, "already subscribed", req.GetType(), { quote });
			return true;
		}
		std::vector<CQuoteInfo> subscriptions{ quote };
		bool bSendUpstream = !m_subscriptions.Subscribe(id, subscriptions).empty();
		bool bPending = false;
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			bPending = m_pendingSubscriptions.end() != m_pendingSubscriptions.find(strKey);
			if (bSendUpstream || bPending)
			{
				m_pendingSubscriptions[strKey].push_back(CPendingSubscription{ id, req.GetId(), quote, std::chrono::steady_clock::now() + UpstreamRequestTimeout });
			}
		}
		if (bSendUpstream)
		{
			if ((nullptr == m_pSession) || !m_pSession->SendRequest(request::Subscription(quote)))
			{
				m_subscriptions.Unsubscribe(id, subscriptions);
				{
					std::lock_guard<std::mutex> lock(m_mtx_state);
					m_pendingSubscriptions.erase(strKey);
				}
				SendSubscriptionResponse(id, req.GetId(), false, "HQMarket is unavailable", req.GetType(), { quote });
				return false;
			}
			return true;
		}
		if (!bPending)
		{
			SendSubscriptionResponse(id, req.GetId(), true, "ok", req.GetType(), { quote });
		}
		return true;
	}

	if (!m_subscriptions.IsSubscribed(id, quote))
	{
		SendSubscriptionResponse(id, req.GetId(), true, "not subscribed", req.GetType(), { quote });
		return true;
	}
	std::vector<CQuoteInfo> subscriptions{ quote };
	bool bSendUpstream = !m_subscriptions.Unsubscribe(id, subscriptions).empty();
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (bSendUpstream)
		{
			m_pendingSubscriptions.erase(strKey);
		}
	}
	if (bSendUpstream && ((nullptr == m_pSession) || !m_pSession->SendRequest(request::UnSubscription(quote))))
	{
		SendSubscriptionResponse(id, req.GetId(), false, "HQMarket is unavailable", req.GetType(), { quote });
		return false;
	}
	SendSubscriptionResponse(id, req.GetId(), true, "ok", req.GetType(), { quote });
	return true;
}

int CHQService::HandleClientDisconnected(net::_TyConnectionId id)
{
	std::vector<CQuoteInfo> removed = m_subscriptions.RemoveClient(id);
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		std::erase_if(m_pendingBatchSubscriptions, [id](const auto& item)
		{
			return id == item.second.m_client.m_routerId;
		});
		for (const auto& quote : removed)
		{
			std::string strKey = quote.String();
			m_pendingSubscriptions.erase(strKey);
		}
		for (auto mIter = m_pendingQueries.begin(); m_pendingQueries.end() != mIter;)
		{
			std::vector<CPendingQueryClient>& clients = mIter->second.m_clients;
			clients.erase(std::remove_if(clients.begin(), clients.end(), [id](const CPendingQueryClient& client)
			{
				return id == client.m_routerId;
			}),
				clients.end());
			if (clients.empty())
			{
				mIter = m_pendingQueries.erase(mIter);
			}
			else
			{
				++mIter;
			}
		}
	}
	if (nullptr != m_pSession)
	{
		for (const auto& quote : removed)
		{
			m_pSession->SendRequest(request::UnSubscription(quote));
		}
	}
	return 1;
}
