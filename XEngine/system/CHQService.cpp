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
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr int InvalidRequest = 1001;
	constexpr int InvalidSubscription = 1006;
	constexpr int UpstreamUnavailable = 1007;
	constexpr int UpstreamTimeout = 1008;
	constexpr int TooManyPendingQueries = 1009;
	constexpr std::size_t MaxPendingQueries = 256;
	constexpr std::size_t MaxPendingSubscriptions = 4096;
	constexpr std::chrono::seconds UpstreamRequestTimeout{ 10 };
} // namespace

CHQService::CHQService(CSession* pSession) : m_bMarketReady((nullptr != pSession) && pSession->IsAuthenticated()), m_pSession(pSession)
{
}

bool CHQService::OnRequest(const CRequest& req)
{
	ExpirePendingQueries();
	ExpirePendingSubscriptions();
	if ((_TyRequest::MARKET_SUBSCRIBE == req.GetType()) || (_TyRequest::MARKET_UNSUBSCRIBE == req.GetType()))
	{
		return OnSubscriptionRequest(req.GetConnectionId(), req);
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
	else if (message.has_trade())
	{
		security = message.trade().security();
		channel = hqmarket::market::v1::CHANNEL_TRADE;
	}
	else if (message.has_bar())
	{
		security = message.bar().security();
		channel = message.bar().channel();
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
		return HandleSubscriptionReply(req);
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

	CRequest push = req;
	push.SetId(0);
	std::vector<std::string> ids = m_subscriptions.GetSubscriberIds(strKey);
	std::vector<net::_TyConnectionId> disconnected;
	for (const std::string& subscriber : ids)
	{
		// Strategy events are delivered by EngineCenter through StrategyService routes.
		HQOrigin type = HQOrigin::Unknown;
		std::uint64_t subscriberId = 0;
		if (!ParseHQSubID(subscriber, type, subscriberId) || (HQOrigin::Client != type) || (static_cast<std::uint64_t>((std::numeric_limits<net::_TyConnectionId>::max)()) < subscriberId))
		{
			continue;
		}
		net::_TyConnectionId id = static_cast<net::_TyConnectionId>(subscriberId);
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
		std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
		m_bMarketReady = true;
		std::vector<CQuoteInfo> quotes = m_subscriptions.GetSubscriptions();
		ReconcileSubscriptions(quotes);
		CompleteSubscriptions();
		return;
	}
	if ((SessionState::Disconnected == state) || (SessionState::Reconnecting == state) || (SessionState::Stopping == state))
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

bool CHQService::HasDemand(const CQuoteInfo& quote) const
{
	return 0 != m_subscriptions.GetSubscriptionCount(quote);
}

void CHQService::ReconcileSubscription(const CQuoteInfo& quote, bool bRefreshSnapshot)
{
	ReconcileSubscriptions({ quote }, bRefreshSnapshot);
}

void CHQService::ReconcileSubscriptions(const std::vector<CQuoteInfo>& quotes, bool bRefreshSnapshot)
{
	if (!m_bMarketReady)
	{
		return;
	}
	std::vector<CQuoteInfo> subscriptions;
	std::vector<CQuoteInfo> unsubscriptions;
	std::unordered_set<std::string> keys;
	for (const auto& quote : quotes)
	{
		if (!keys.emplace(quote.String()).second)
		{
			continue;
		}
		CUpstreamSubscription& state = m_upstreamSubscriptions[quote.String()];
		state.m_quote = quote;
		bool bSubscribe = HasDemand(quote);
		bool bSnapshot = bRefreshSnapshot && bSubscribe && (Channel::quote == quote.m_channel);
		if ((0 != state.m_requestId) || ((bSubscribe == state.m_bConfirmed) && !state.m_bUncertain && !bSnapshot))
		{
			continue;
		}
		(bSubscribe ? subscriptions : unsubscriptions).emplace_back(quote);
	}
	for (const std::vector<CQuoteInfo>* pGroup : { &subscriptions, &unsubscriptions })
	{
		const std::vector<CQuoteInfo>& pending = *pGroup;
		bool bSubscribe = &subscriptions == pGroup;
		constexpr std::size_t BatchSize = 1000;
		for (std::size_t nOffset = 0; pending.size() > nOffset; nOffset += BatchSize)
		{
			std::size_t nEnd = (std::min)(pending.size(), nOffset + BatchSize);
			CRequest request = bSubscribe ? request::Subscription(pending[nOffset]) : request::UnSubscription(pending[nOffset]);
			_TyRequestId requestId = m_routerId.fetch_add(1);
			request.SetId(requestId);
			CRequest::_TyExtraRows rows;
			rows.reserve(nEnd - nOffset);
			for (std::size_t nIndex = nOffset; nEnd > nIndex; ++nIndex)
			{
				const CQuoteInfo& quote = pending[nIndex];
				CUpstreamSubscription& state = m_upstreamSubscriptions.at(quote.String());
				state.m_requestId = requestId;
				state.m_type = request.GetType();
				state.m_deadline = std::chrono::steady_clock::now() + UpstreamRequestTimeout;
				rows.emplace_back(std::unordered_map<std::string, std::string>{ { "security", quote.m_security.String() }, { "channel", GetChannelString(quote.m_channel) } });
			}
			if (1 < rows.size())
			{
				request.SetExtraData("subscriptions", rows);
			}
			if ((nullptr == m_pSession) || !m_pSession->SendRequest(request))
			{
				for (std::size_t nIndex = nOffset; nEnd > nIndex; ++nIndex)
				{
					FinishSubscription(pending[nIndex].String(), false, "HQMarket is unavailable", true);
				}
			}
		}
	}
}

void CHQService::CompleteSubscriptions()
{
	for (auto mIter = m_pendingBatchSubscriptions.begin(); m_pendingBatchSubscriptions.end() != mIter;)
	{
		CPendingSubscriptionBatch& pending = mIter->second;
		bool bComplete = !pending.m_strError.empty();
		if (!bComplete)
		{
			bComplete = std::all_of(pending.m_requested.begin(), pending.m_requested.end(), [this, &pending](const CQuoteInfo& quote)
			{
				const auto stateIter = m_upstreamSubscriptions.find(quote.String());
				if (_TyRequest::MARKET_SUBSCRIBE == pending.m_type)
				{
					return (m_upstreamSubscriptions.end() != stateIter) && stateIter->second.m_bConfirmed && !stateIter->second.m_bUncertain && (0 == stateIter->second.m_requestId);
				}
				return HasDemand(quote) || (m_upstreamSubscriptions.end() == stateIter) || (!stateIter->second.m_bConfirmed && !stateIter->second.m_bUncertain && 0 == stateIter->second.m_requestId);
			});
		}
		if (!bComplete)
		{
			++mIter;
			continue;
		}
		SendSubscriptionResponse(pending.m_client.m_routerId, pending.m_client.m_requestId, pending.m_strError.empty(), pending.m_strError, pending.m_type, pending.m_requested);
		mIter = m_pendingBatchSubscriptions.erase(mIter);
	}
	std::erase_if(m_upstreamSubscriptions, [this](const auto& item)
	{
		return (0 == item.second.m_requestId) && !item.second.m_bConfirmed && !item.second.m_bUncertain && !HasDemand(item.second.m_quote);
	});
}

void CHQService::FinishSubscription(const std::string& strKey, bool bAccepted, const std::string& strReason, bool bUncertain)
{
	const auto stateIter = m_upstreamSubscriptions.find(strKey);
	if (m_upstreamSubscriptions.end() == stateIter)
	{
		return;
	}
	CUpstreamSubscription& state = stateIter->second;
	_TyRequest type = state.m_type;
	state.m_requestId = 0;
	state.m_bUncertain = bUncertain || (!bAccepted && state.m_bUncertain);
	if (bAccepted)
	{
		state.m_bConfirmed = _TyRequest::MARKET_SUBSCRIBE == type;
		ReconcileSubscription(state.m_quote);
		return;
	}
	// Retain strategy demand for reconnect; terminal requests receive the failure.
	for (auto& item : m_pendingBatchSubscriptions)
	{
		CPendingSubscriptionBatch& pending = item.second;
		bool bAffected = std::any_of(pending.m_requested.begin(), pending.m_requested.end(), [&strKey](const CQuoteInfo& quote)
		{
			return strKey == quote.String();
		});
		if (!bAffected || !pending.m_strError.empty())
		{
			continue;
		}
		pending.m_strError = strReason.empty() ? "HQMarket rejected subscription" : strReason;
		if (_TyRequest::MARKET_SUBSCRIBE == pending.m_type)
		{
			m_subscriptions.Unsubscribe(FmtHQSubID(HQOrigin::Client, static_cast<std::uint64_t>(pending.m_client.m_routerId)), pending.m_changed);
		}
		else
		{
			m_subscriptions.Subscribe(FmtHQSubID(HQOrigin::Client, static_cast<std::uint64_t>(pending.m_client.m_routerId)), pending.m_changed);
		}
	}
	if (bUncertain && (_TyRequest::MARKET_SUBSCRIBE == type) && !HasDemand(state.m_quote))
	{
		// The timed-out/failed write may have reached HQMarket. Release it explicitly.
		ReconcileSubscription(state.m_quote);
	}
}

bool CHQService::HandleSubscriptionReply(const CRequest& req)
{
	std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
	std::unordered_set<std::string> keys;
	for (const auto& item : m_upstreamSubscriptions)
	{
		if ((0 != item.second.m_requestId) && (req.GetId() == item.second.m_requestId) && (req.GetType() == item.second.m_type))
		{
			keys.emplace(item.first);
		}
	}
	if (keys.empty())
	{
		return false;
	}
	std::optional<std::pair<int, std::string>> error = req.GetErrorInfo();
	std::unordered_map<std::string, std::pair<bool, std::string>> results;
	const _TyReqData& data = req.GetData();
	if (data.has_subscription_ack())
	{
		for (const auto& result : data.subscription_ack().results())
		{
			CQuoteInfo quote(result.security().symbol(), static_cast<Exchange>(result.security().exchange()), static_cast<Channel>(result.channel()));
			results.insert_or_assign(quote.String(), std::make_pair(result.accepted(), result.reason()));
		}
	}
	for (const auto& strKey : keys)
	{
		bool bAccepted = !error.has_value();
		std::string strReason = error.has_value() ? error->second : std::string();
		if (data.has_subscription_ack())
		{
			const auto resultIter = results.find(strKey);
			if (results.end() == resultIter)
			{
				bAccepted = false;
				strReason = "HQMarket subscription ACK is missing a result";
			}
			else
			{
				bAccepted = bAccepted && resultIter->second.first;
				if (!resultIter->second.first)
				{
					strReason = resultIter->second.second;
				}
			}
		}
		FinishSubscription(strKey, bAccepted, strReason);
	}
	// A failed batch may release other confirmed keys; converge all remaining demand.
	std::vector<CQuoteInfo> quotes;
	for (const auto& item : m_upstreamSubscriptions)
	{
		if (!keys.contains(item.first))
		{
			quotes.emplace_back(item.second.m_quote);
		}
	}
	ReconcileSubscriptions(quotes);
	CompleteSubscriptions();
	return true;
}
void CHQService::ExpirePendingSubscriptions()
{
	std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
	std::vector<std::string> expired;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	for (const auto& item : m_upstreamSubscriptions)
	{
		if ((0 != item.second.m_requestId) && (item.second.m_deadline <= now))
		{
			expired.emplace_back(item.first);
		}
	}
	for (const auto& strKey : expired)
	{
		FinishSubscription(strKey, false, "HQMarket subscription timeout", true);
	}
	CompleteSubscriptions();
}

void CHQService::FailPendingSubscriptions(const std::string& strReason)
{
	std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
	m_bMarketReady = false;
	for (auto& item : m_pendingBatchSubscriptions)
	{
		CPendingSubscriptionBatch& pending = item.second;
		pending.m_strError = strReason;
		if (_TyRequest::MARKET_SUBSCRIBE == pending.m_type)
		{
			m_subscriptions.Unsubscribe(FmtHQSubID(HQOrigin::Client, static_cast<std::uint64_t>(pending.m_client.m_routerId)), pending.m_changed);
		}
		else
		{
			m_subscriptions.Subscribe(FmtHQSubID(HQOrigin::Client, static_cast<std::uint64_t>(pending.m_client.m_routerId)), pending.m_changed);
		}
	}
	for (auto& item : m_upstreamSubscriptions)
	{
		item.second.m_requestId = 0;
		item.second.m_bConfirmed = false;
		item.second.m_bUncertain = false;
	}
	CompleteSubscriptions();
}

void CHQService::Subscribe(HQOrigin type, std::uint64_t id, const std::vector<CQuoteInfo>& quotes)
{
	std::string strKey = FmtHQSubID(type, id);
	if (strKey.empty() || quotes.empty() || !std::all_of(quotes.begin(), quotes.end(), [](const CQuoteInfo& quote)
	{
		return quote.IsValid();
	}))
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
	m_subscriptions.Subscribe(strKey, quotes);
	ReconcileSubscriptions(quotes);
	CompleteSubscriptions();
}

void CHQService::Release(HQOrigin type, std::uint64_t id)
{
	std::string strKey = FmtHQSubID(type, id);
	if (strKey.empty())
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
	std::vector<CQuoteInfo> quotes = m_subscriptions.RemoveSubscriber(strKey);
	if (HQOrigin::Client == type)
	{
		std::erase_if(m_pendingBatchSubscriptions, [id](const auto& item)
		{
			return id == static_cast<std::uint64_t>(item.second.m_client.m_routerId);
		});
	}
	ReconcileSubscriptions(quotes);
	CompleteSubscriptions();
}

bool CHQService::OnSubscriptionRequest(net::_TyConnectionId id, const CRequest& req)
{
	std::string strKey = FmtHQSubID(HQOrigin::Client, static_cast<std::uint64_t>(id));
	bool bSubscribe = request::IsSubscriptionRequest(req);
	if (!bSubscribe && !request::IsUnSubscriptionRequest(req))
	{
		net::SendError(id, req, InvalidSubscription, "unsupported request");
		return false;
	}
	std::vector<CQuoteInfo> quotes;
	if (req.GetExtraData("subscriptions").empty())
	{
		quotes.emplace_back(GetQuoteInfo(req));
	}
	else
	{
		CRequest::_TyExtraRows rows;
		if (!req.GetExtraData("subscriptions", rows) || rows.empty())
		{
			net::SendError(id, req, InvalidSubscription, "subscriptions must be a nonempty JSON array");
			return false;
		}
		quotes.reserve(rows.size());
		for (const auto& row : rows)
		{
			const auto securityIter = row.find("security");
			const auto channelIter = row.find("channel");
			if ((row.end() == securityIter) || (row.end() == channelIter))
			{
				net::SendError(id, req, InvalidSubscription, "subscription row requires security and channel");
				return false;
			}
			CRequest item;
			item.SetExtraData("security", securityIter->second);
			item.SetExtraData("channel", channelIter->second);
			quotes.emplace_back(GetQuoteInfo(item));
		}
	}
	for (const auto& quote : quotes)
	{
		if (!quote.IsValid())
		{
			net::SendError(id, req, InvalidSubscription, "invalid security or unsupported subscription channel");
			return false;
		}
	}
	std::lock_guard<std::mutex> lock(m_mtx_subscriptions);
	if (!m_bMarketReady && bSubscribe)
	{
		SendSubscriptionResponse(id, req.GetId(), false, "HQMarket is unavailable", req.GetType(), quotes);
		return false;
	}
	if (MaxPendingSubscriptions <= m_pendingBatchSubscriptions.size())
	{
		SendSubscriptionResponse(id, req.GetId(), false, "too many pending subscriptions", req.GetType(), quotes);
		return false;
	}
	std::vector<CQuoteInfo> superseded;
	for (auto& item : m_pendingBatchSubscriptions)
	{
		CPendingSubscriptionBatch& previous = item.second;
		if ((id != previous.m_client.m_routerId) || (req.GetType() == previous.m_type) || !previous.m_strError.empty())
		{
			continue;
		}
		bool bOverlaps = std::any_of(previous.m_requested.begin(), previous.m_requested.end(), [&quotes](const CQuoteInfo& previousQuote)
		{
			return std::any_of(quotes.begin(), quotes.end(), [&previousQuote](const CQuoteInfo& quote)
			{
				return previousQuote.String() == quote.String();
			});
		});
		if (bOverlaps)
		{
			previous.m_strError = "subscription request superseded";
			if (_TyRequest::MARKET_SUBSCRIBE == previous.m_type)
			{
				m_subscriptions.Unsubscribe(strKey, previous.m_changed);
			}
			else
			{
				m_subscriptions.Subscribe(strKey, previous.m_changed);
			}
			superseded.insert(superseded.end(), previous.m_changed.begin(), previous.m_changed.end());
		}
	}
	CPendingSubscriptionBatch pending;
	pending.m_client = { id, req.GetId() };
	pending.m_type = req.GetType();
	pending.m_requested = quotes;
	for (const auto& quote : quotes)
	{
		if (bSubscribe != m_subscriptions.IsSubscribed(strKey, quote))
		{
			pending.m_changed.emplace_back(quote);
		}
	}
	if (bSubscribe)
	{
		m_subscriptions.Subscribe(strKey, quotes);
	}
	else
	{
		m_subscriptions.Unsubscribe(strKey, quotes);
	}
	m_pendingBatchSubscriptions.emplace(m_routerId.fetch_add(1), std::move(pending));
	ReconcileSubscriptions(quotes, bSubscribe);
	ReconcileSubscriptions(superseded);
	CompleteSubscriptions();
	return true;
}

int CHQService::HandleClientDisconnected(net::_TyConnectionId id)
{
	Release(HQOrigin::Client, static_cast<std::uint64_t>(id));
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pendingQueries.begin(); m_pendingQueries.end() != mIter;)
		{
			std::erase_if(mIter->second.m_clients, [id](const CPendingQueryClient& client)
			{
				return id == client.m_routerId;
			});
			mIter = mIter->second.m_clients.empty() ? m_pendingQueries.erase(mIter) : std::next(mIter);
		}
	}
	return 1;
}
