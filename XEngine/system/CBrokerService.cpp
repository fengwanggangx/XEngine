#include "CBrokerService.h"

#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../network/CNetTools.h"
#include "../network/CTcpServer.h"
#include "../network/common_net.h"
#include "../request/RequestCenter.h"
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../request/v1/market.pb.h"
#include "../strategy/CStrategyEngine.h"
#include "CSession.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr int InvalidRequest = 1001;
	constexpr int InvalidCredentials = 1002;
	constexpr int AccountExists = 1003;
	constexpr int StorageUnavailable = 1004;
	constexpr std::size_t MinAccountLength = 3;
	constexpr std::size_t MaxAccountLength = 64;
	constexpr std::size_t MinPasswordLength = 8;
	constexpr std::size_t MaxPasswordLength = 128;
	constexpr int AuthenticationRequired = 1005;
	constexpr int InvalidSubscription = 1006;
	constexpr int UpstreamUnavailable = 1007;
	constexpr int UpstreamTimeout = 1008;
	constexpr int TooManyPendingQueries = 1009;
	constexpr std::size_t MaxPendingQueries = 256;
	constexpr std::chrono::hours TokenLifetime{ 24 };
	constexpr std::chrono::seconds UpstreamRequestTimeout{ 10 };
} // namespace

bool IsAccountValid(const std::string& strAccount)
{
	if ((MinAccountLength > strAccount.size()) || (MaxAccountLength < strAccount.size()))
	{
		return false;
	}
	for (unsigned char c : strAccount)
	{
		if ((0 == std::isalnum(c)) && ('_' != c) && ('-' != c) && ('.' != c) && ('@' != c))
		{
			return false;
		}
	}
	return true;
}

bool IsPasswordValid(const std::string& strPassword)
{
	return (MinPasswordLength <= strPassword.size()) && (MaxPasswordLength >= strPassword.size());
}

CBrokerService::CBrokerService(net::CTcpServer* pTcpServer, CSession* pSession, CStrategyEngine* pStrategyEngine) : m_pTcpServer(pTcpServer), m_pSession(pSession), m_pStrategyEngine(pStrategyEngine)
{
	m_handler = {
		{ _TyRequest::TRADE_CAPABILITIES_QUERY, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::TRADE_ORDER_SUBMIT, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::TRADE_ORDER_CANCEL, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::TRADE_POSITION_QUERY, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::TRADE_ORDER_QUERY, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::TRADE_TRADE_QUERY, std::bind_front(&CBrokerService::HandleTrading, this) },

		{ _TyRequest::AUTH_REGISTER, std::bind_front(&CBrokerService::HandleRegisterAuth, this) },
		{ _TyRequest::AUTH_LOGIN, std::bind_front(&CBrokerService::HandleAuth, this) },
		{ _TyRequest::HEARTBEAT, std::bind_front(&CBrokerService::HandleHeartbeat, this) },
		{ _TyRequest::MARKET_SUBSCRIBE, std::bind_front(&CBrokerService::HandleSubscription, this) },
		{ _TyRequest::MARKET_UNSUBSCRIBE, std::bind_front(&CBrokerService::HandleSubscription, this) },
		{ _TyRequest::MARKET_QUOTE_QUERY, std::bind_front(&CBrokerService::HandleMarketQuery, this) },
		{ _TyRequest::MARKET_BARS_QUERY, std::bind_front(&CBrokerService::HandleMarketQuery, this) },
		{ _TyRequest::MARKET_SECURITIES_QUERY, std::bind_front(&CBrokerService::HandleMarketQuery, this) },
		{ _TyRequest::MARKET_SECTORS_QUERY, std::bind_front(&CBrokerService::HandleMarketQuery, this) },
		{ _TyRequest::MARKET_SECTOR_CONSTITUENTS_QUERY, std::bind_front(&CBrokerService::HandleMarketQuery, this) },
		{ _TyRequest::BROKER_QUERY, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::SIMULATED_ACCOUNT_OPEN, std::bind_front(&CBrokerService::HandleTrading, this) },
		{ _TyRequest::TRADE_ACCOUNT_LOGIN, std::bind_front(&CBrokerService::HandleAccount, this) },
		{ _TyRequest::TRADE_ACCOUNT_QUERY, std::bind_front(&CBrokerService::HandleAccount, this) },
		{ _TyRequest::TRADE_ACCOUNT_DISCONNECT, std::bind_front(&CBrokerService::HandleAccount, this) },
		{ _TyRequest::TRADE_ACCOUNT_INFO_QUERY, std::bind_front(&CBrokerService::HandleAccount, this) },
		{ _TyRequest::STRATEGY_ADD, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_MODIFY, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_QUERY, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_DELETE, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_RUNTIME_QUERY, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_START, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_PAUSE, std::bind_front(&CBrokerService::HandleStrategy, this) },
		{ _TyRequest::STRATEGY_STOP, std::bind_front(&CBrokerService::HandleStrategy, this) }
	};
}

bool CBrokerService::Initialize()
{
	if ((nullptr == m_pTcpServer) || (nullptr == m_pSession) || (nullptr == m_pStrategyEngine))
	{
		return false;
	}
	m_pStrategyEngine->SetTradingPushHandler([this](const CRequest& push)
	{
		std::vector<net::_TyConnectionId> clients;
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			clients.assign(m_auth_clients.begin(), m_auth_clients.end());
		}
		for (net::_TyConnectionId id : clients)
		{
			net::SendRequest(id, push);
		}
	});
	m_pTcpServer->RegisterHandler(std::bind_front(&CBrokerService::OnNetEvent, this));
	m_pSession->RegisterHandler(std::bind_front(&CBrokerService::OnHQMarketResponse, this));
	m_pSession->RegisterStateHandler(std::bind_front(&CBrokerService::OnHQMarketState, this));
	return true;
}

CQuoteInfo CBrokerService::GetQuoteInfo(const CRequest& req) const
{
	std::string strSecurity = req.GetExtraData("security");
	std::size_t nDot = strSecurity.rfind('.');
	if ((std::string::npos == nDot) || (0 == nDot) || (strSecurity.size() - 1 == nDot))
	{
		return {};
	}
	return CQuoteInfo(strSecurity.substr(0, nDot), ParseMarket(strSecurity.substr(nDot + 1)), ParseChannel(req.GetExtraData("channel")));
}

void CBrokerService::SendSubscriptionResponse(net::_TyConnectionId id, _TyRequestId requestId, bool bAccepted, const std::string& strReason, _TyRequest type, const std::vector<CQuoteInfo>& subscriptions) const
{
	CRequest response;
	response.SetId(requestId);
	response.SetType(type);
	response.SetErrorInfo(bAccepted ? 0 : InvalidSubscription, bAccepted ? "" : strReason);
	hqmarket::market::v1::SubscriptionAck ack;
	for (const auto& quote : subscriptions)
	{
		hqmarket::market::v1::SubscriptionResult* pResult = ack.add_results();
		pResult->mutable_security()->set_symbol(quote.m_security.m_strCode);
		pResult->mutable_security()->set_exchange(static_cast<hqmarket::market::v1::Exchange>(quote.m_security.m_market));
		pResult->set_channel(static_cast<hqmarket::market::v1::Channel>(quote.m_channel));
		pResult->set_accepted(bAccepted);
		pResult->set_reason(bAccepted ? "" : strReason);
	}
	response.SetData(ack);
	net::SendRequest(id, response);
}

std::string CBrokerService::GetMarketResponseKey(const CRequest& req) const
{
	const _TyReqData& message = req.GetData();
	hqmarket::market::v1::Security security;
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

void CBrokerService::OnHQMarketResponse(const CRequest& req)
{
	ExpirePendingSubscriptions();
	ExpirePendingQueries();
	if ((_TyRequest::MARKET_SUBSCRIBE == req.GetType()) || (_TyRequest::MARKET_UNSUBSCRIBE == req.GetType()))
	{
		CPendingSubscriptionBatch pending;
		bool bBatch = false;
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			auto mIter = m_pending_batch_subscriptions.find(req.GetId());
			if ((m_pending_batch_subscriptions.end() != mIter) && (req.GetType() == mIter->second.m_type))
			{
				pending = std::move(mIter->second);
				m_pending_batch_subscriptions.erase(mIter);
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
					m_subscriptions.Unsubscribe(pending.m_client.m_router_id, pending.m_changed);
				}
				else
				{
					m_subscriptions.Subscribe(pending.m_client.m_router_id, pending.m_changed);
				}
			}
			SendSubscriptionResponse(pending.m_client.m_router_id, pending.m_client.m_requestId, !error.has_value(), error.has_value() ? error->second : "", pending.m_type, pending.m_requested);
			return;
		}
	}
	if (RouteQueryRequest(req))
	{
		return;
	}
	std::string strKey = GetMarketResponseKey(req);
	if (strKey.empty())
	{
		return;
	}

	_TyRequest type = req.GetType();
	if ((_TyRequest::MARKET_SUBSCRIBE == type) || (_TyRequest::MARKET_UNSUBSCRIBE == type))
	{
		std::vector<PendingSubscription> pending;
		std::optional<std::pair<int, std::string>> error = req.GetErrorInfo();
		bool bAccepted = !error.has_value();
		std::string strReason = error.has_value() ? error->second : std::string();
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			auto mIter = m_pending_subscriptions.find(strKey);
			if (m_pending_subscriptions.end() == mIter)
			{
				return;
			}
			pending = std::move(mIter->second);
			m_pending_subscriptions.erase(mIter);
		}
		if (!bAccepted)
		{
			m_subscriptions.RemoveSubscription(strKey);
		}
		for (const auto& v : pending)
		{
			SendSubscriptionResponse(v.m_router_id, v.m_requestId, bAccepted, strReason, _TyRequest::MARKET_SUBSCRIBE, { v.m_quote });
		}
		return;
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
		HandleDisconnected(id);
	}
}

void CBrokerService::OnHQMarketState(SessionState state, const std::string& strMessage)
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

std::string CBrokerService::GetQueryKey(const CRequest& req) const
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

bool CBrokerService::HandleMarketQuery(net::_TyConnectionId id, const CRequest& req)
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
		auto mIter = m_pending_queries.find(strKey);
		if (m_pending_queries.end() != mIter)
		{
			mIter->second.m_clients.emplace_back(CPendingQueryClient{ id, req.GetId() });
			return true;
		}
		if (MaxPendingQueries <= m_pending_queries.size())
		{
			bTooManyPending = true;
		}
		else
		{
			routerId = m_router_id.fetch_add(1);
			CPendingQuery pending;
			pending.m_router_id = routerId;
			pending.m_strKey = strKey;
			pending.m_type = req.GetType();
			pending.m_deadline = std::chrono::steady_clock::now() + UpstreamRequestTimeout;
			pending.m_clients.emplace_back(CPendingQueryClient{ id, req.GetId() });
			m_pending_queries.emplace(strKey, std::move(pending));
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
		auto mIter = m_pending_queries.find(strKey);
		if ((m_pending_queries.end() != mIter) && (routerId == mIter->second.m_router_id))
		{
			failed = std::move(mIter->second);
			m_pending_queries.erase(mIter);
		}
	}
	for (const auto& client : failed.m_clients)
	{
		SendQueryError(client, failed.m_type, UpstreamUnavailable, "HQMarket is unavailable");
	}
	return false;
}

bool CBrokerService::RouteQueryRequest(const CRequest& req)
{
	CPendingQuery pending;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		_TyRequestId routerId = req.GetId();
		auto mIter = std::find_if(m_pending_queries.begin(), m_pending_queries.end(), [routerId](const auto& item)
		{
			return routerId == item.second.m_router_id;
		});
		if ((m_pending_queries.end() == mIter) || (req.GetType() != mIter->second.m_type))
		{
			return false;
		}
		pending = std::move(mIter->second);
		m_pending_queries.erase(mIter);
	}
	for (const auto& client : pending.m_clients)
	{
		CRequest s = req;
		s.SetId(client.m_requestId);
		net::SendRequest(client.m_router_id, s);
	}
	return true;
}

void CBrokerService::SendQueryError(const CPendingQueryClient& client, _TyRequest type, int nErrorCode, const std::string& strMessage) const
{
	CRequest response;
	response.SetId(client.m_requestId);
	response.SetType(type);
	net::SetError(response, nErrorCode, strMessage);
	net::SendRequest(client.m_router_id, response);
}

void CBrokerService::ExpirePendingQueries()
{
	std::vector<CPendingQuery> expired;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pending_queries.begin(); m_pending_queries.end() != mIter;)
		{
			if (mIter->second.m_deadline <= now)
			{
				expired.emplace_back(std::move(mIter->second));
				mIter = m_pending_queries.erase(mIter);
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

void CBrokerService::FailPendingQueries(const std::string& strReason)
{
	std::vector<CPendingQuery> failed;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		failed.reserve(m_pending_queries.size());
		for (auto& item : m_pending_queries)
		{
			failed.emplace_back(std::move(item.second));
		}
		m_pending_queries.clear();
	}
	for (const auto& pending : failed)
	{
		for (const auto& client : pending.m_clients)
		{
			SendQueryError(client, pending.m_type, UpstreamUnavailable, strReason);
		}
	}
}

void CBrokerService::ExpirePendingSubscriptions()
{
	std::vector<CPendingSubscriptionBatch> expiredBatches;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pending_batch_subscriptions.begin(); m_pending_batch_subscriptions.end() != mIter;)
		{
			if (mIter->second.m_deadline <= std::chrono::steady_clock::now())
			{
				expiredBatches.emplace_back(std::move(mIter->second));
				mIter = m_pending_batch_subscriptions.erase(mIter);
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
			m_subscriptions.Unsubscribe(pending.m_client.m_router_id, pending.m_changed);
		}
		else
		{
			m_subscriptions.Subscribe(pending.m_client.m_router_id, pending.m_changed);
		}
		SendSubscriptionResponse(pending.m_client.m_router_id, pending.m_client.m_requestId, false, "HQMarket subscription timeout", pending.m_type, pending.m_requested);
	}
	std::vector<PendingSubscription> expired;
	std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto mIter = m_pending_subscriptions.begin(); m_pending_subscriptions.end() != mIter;)
		{
			auto& values = mIter->second;
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
			mIter = values.empty() ? m_pending_subscriptions.erase(mIter) : std::next(mIter);
		}
	}
	for (const auto& pending : expired)
	{
		m_subscriptions.Unsubscribe(pending.m_router_id, { pending.m_quote });
		if (nullptr != m_pSession)
		{
			m_pSession->SendRequest(request::UnSubscription(pending.m_quote));
		}
		SendSubscriptionResponse(pending.m_router_id, pending.m_requestId, false, "HQMarket request timeout");
	}
}

void CBrokerService::FailPendingSubscriptions(const std::string& strReason)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (auto& item : m_pending_batch_subscriptions)
		{
			item.second.m_deadline = std::chrono::steady_clock::time_point::min();
		}
	}
	ExpirePendingSubscriptions();
	std::vector<PendingSubscription> failed;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (const auto& [k, v] : m_pending_subscriptions)
		{
			failed.insert(failed.end(), v.begin(), v.end());
		}
		m_pending_subscriptions.clear();
	}
	for (const auto& pending : failed)
	{
		m_subscriptions.Unsubscribe(pending.m_router_id, { pending.m_quote });
		if (nullptr != m_pSession)
		{
			m_pSession->SendRequest(request::UnSubscription(pending.m_quote));
		}
		SendSubscriptionResponse(pending.m_router_id, pending.m_requestId, false, strReason);
	}
}

void CBrokerService::SendResponse(const CRequest& req, int nErrorCode, const std::string& strMessage) const
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	if (0 != nErrorCode)
	{
		net::SetError(response, nErrorCode, strMessage);
	}
	else
	{

		response.SetErrorInfo(0, "");
	}
	net::SendRequest(req.GetConnectionId(), response);
}

bool CBrokerService::Login(const CRequest& req, std::string& strToken)
{
	std::string strAccount = req.GetExtraData("user");
	std::string strPassword = req.GetExtraData("password");
	if (!IsAccountValid(strAccount) || !IsPasswordValid(strPassword))
	{
		SendResponse(req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == db)
	{
		SendResponse(req, StorageUnavailable, "用户数据库暂不可用");
		return false;
	}

	std::string strSql = "SELECT user_id, account FROM table_user WHERE account=" + utility::Utf8Literal(strAccount) + " AND password_hash=UNHEX(SHA2(CONCAT(password_salt,UNHEX('" + utility::ToHex(strPassword) + "')),256)) AND status=1 LIMIT 1";
	const db::CTable& table = db->ExecQuery(strSql);
	if (table.m_rows.empty())
	{
		SendResponse(req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());

	response.SetExtraData("user_id", db::to_string(table.m_rows.front().at(0)));
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_clientUsers.insert_or_assign(req.GetConnectionId(), strAccount);
	}
	response.SetExtraData("account", db::to_string(table.m_rows.front().at(1)));
	strToken = utility::MakeSaltHex();
	response.SetExtraData("token", strToken);
	net::SendRequest(req.GetConnectionId(), response);
	return true;
}

bool CBrokerService::HandleRegisterAuth(net::_TyConnectionId, const CRequest& req)
{
	std::string strAccount = req.GetExtraData("user");
	std::string strPassword = req.GetExtraData("password");
	if (!IsAccountValid(strAccount))
	{
		SendResponse(req, InvalidRequest, "账号需为 3-64 位字母、数字或 _-.@");
		return false;
	}
	if (!IsPasswordValid(strPassword))
	{
		SendResponse(req, InvalidRequest, "密码长度需为 8-128 位");
		return false;
	}

	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == db)
	{
		SendResponse(req, StorageUnavailable, "用户数据库暂不可用");
		return false;
	}

	std::string strAccountLiteral = utility::Utf8Literal(strAccount);
	const db::CTable& table = db->ExecQuery("SELECT user_id FROM table_user WHERE account=" + strAccountLiteral + " LIMIT 1");
	if (!table.m_rows.empty())
	{
		SendResponse(req, AccountExists, "账号已存在");
		return false;
	}

	std::string saltHex = utility::MakeSaltHex();
	std::string strSQL = "INSERT INTO table_user(account,password_hash,password_salt,status) VALUES(" + strAccountLiteral + ",UNHEX(SHA2(CONCAT(UNHEX('" + saltHex + "'),UNHEX('" + utility::ToHex(strPassword) + "')),256)),UNHEX('" + saltHex + "'),1)";
	if (0 != db->ExecUpdate(strSQL))
	{
		SendResponse(req, AccountExists, "账号已存在或注册失败");
		return false;
	}

	SendResponse(req, 0, "注册成功");
	return true;
}

bool CBrokerService::HandleSubscription(net::_TyConnectionId, const CRequest& req)
{
	net::_TyConnectionId id = static_cast<net::_TyConnectionId>(req.GetConnectionId());
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (m_auth_clients.end() == m_auth_clients.find(id))
		{
			net::SendError(id, req, AuthenticationRequired, "authentication required");
			return false;
		}
	}

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
			auto security = row.find("security");
			auto channel = row.find("channel");
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
		_TyRequestId routerId = m_router_id.fetch_add(1);
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
			m_pending_batch_subscriptions.emplace(routerId, CPendingSubscriptionBatch{ { id, req.GetId() }, req.GetType(), std::chrono::steady_clock::now() + UpstreamRequestTimeout, subscriptions, applied });
		}
		if ((nullptr != m_pSession) && m_pSession->SendRequest(router))
		{
			return true;
		}
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			m_pending_batch_subscriptions.erase(routerId);
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
			bPending = m_pending_subscriptions.end() != m_pending_subscriptions.find(strKey);
			if (bSendUpstream || bPending)
			{
				m_pending_subscriptions[strKey].push_back(PendingSubscription{ id, req.GetId(), quote, std::chrono::steady_clock::now() + UpstreamRequestTimeout });
			}
		}
		if (bSendUpstream)
		{
			if ((nullptr == m_pSession) || !m_pSession->SendRequest(request::Subscription(quote)))
			{
				m_subscriptions.Unsubscribe(id, subscriptions);
				{
					std::lock_guard<std::mutex> lock(m_mtx_state);
					m_pending_subscriptions.erase(strKey);
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
			m_pending_subscriptions.erase(strKey);
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

int CBrokerService::HandleDisconnected(net::_TyConnectionId id)
{
	std::vector<CQuoteInfo> removed = m_subscriptions.RemoveClient(id);
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_auth_clients.erase(id);
		m_clientUsers.erase(id);
		std::erase_if(m_pending_batch_subscriptions, [id](const auto& item)
		{
			return id == item.second.m_client.m_router_id;
		});
		for (const auto& quote : removed)
		{
			std::string strKey = quote.String();
			m_pending_subscriptions.erase(strKey);
		}
		for (auto mIter = m_pending_queries.begin(); m_pending_queries.end() != mIter;)
		{
			std::vector<CPendingQueryClient>& clients = mIter->second.m_clients;
			clients.erase(std::remove_if(clients.begin(), clients.end(), [id](const CPendingQueryClient& client)
			{
				return id == client.m_router_id;
			}),
				clients.end());
			if (clients.empty())
			{
				mIter = m_pending_queries.erase(mIter);
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

bool CBrokerService::HandleReAuth(const CRequest& req)
{
	std::string strToken = req.GetExtraData("token");
	bool bAccepted = false;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		auto mIter = m_client_tokens.find(strToken);
		bAccepted = (m_client_tokens.end() != mIter) && (std::chrono::steady_clock::now() < mIter->second);
		if ((m_client_tokens.end() != mIter) && !bAccepted)
		{
			m_client_tokens.erase(mIter);
			m_tokenUsers.erase(strToken);
		}
		if (bAccepted)
		{
			m_auth_clients.emplace(req.GetConnectionId());
			m_clientUsers.insert_or_assign(req.GetConnectionId(), m_tokenUsers.at(strToken));
		}
	}
	if (bAccepted)
	{
		SendResponse(req, 0, "认证成功");
		return true;
	}
	SendResponse(req, InvalidCredentials, "登录状态已失效");
	return false;
}

bool CBrokerService::IsAuthenticated(net::_TyConnectionId id) const
{
	std::lock_guard<std::mutex> lck(m_mtx_state);
	return (m_auth_clients.end() != m_auth_clients.find(id));
}

bool CBrokerService::HandleAuth(net::_TyConnectionId id, const CRequest& req)
{
	// 断线重连
	std::string strToken = req.GetExtraData("token");
	if (!strToken.empty())
	{
		return HandleReAuth(req);
	}

	// 登录认证
	if (request::IsAuthRequest(req))
	{
		if (Login(req, strToken))
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			m_auth_clients.emplace(req.GetConnectionId());
			m_tokenUsers.insert_or_assign(strToken, m_clientUsers.at(req.GetConnectionId()));
			m_client_tokens.insert_or_assign(strToken, std::chrono::steady_clock::now() + TokenLifetime);
		}
		return true;
	}
	net::SendError(req.GetConnectionId(), req, InvalidRequest, "unsupported authentication request");
	return false;
}

void CBrokerService::OnClientRequest(net::_TyConnectionId id, const CRequest& req)
{
	ExpirePendingQueries();
	_TyRequest type = req.GetType();
	const auto mIter = m_handler.find(type);
	if (m_handler.end() == mIter)
	{
		net::SendError(id, req, 1006, "unknown request type");
		return;
	}

	if (request::IsAuthRequest(req) || request::IsRegisterAccountRequest(req))
	{
		mIter->second(id, req);
		return;
	}

	if (!IsAuthenticated(id))
	{
		net::SendError(id, req, 1002, "authentication required");
		return;
	}

	mIter->second(id, req);
}

bool CBrokerService::HandleHeartbeat(net::_TyConnectionId id, const CRequest& req)
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(_TyRequest::HEARTBEAT);
	response.SetExtraData("client_time_ms", req.GetExtraData("client_time_ms"));

	return net::SendRequest(id, response);
}

bool CBrokerService::HandleStrategy(net::_TyConnectionId id, const CRequest& req)
{
	if ((!req.IsStrategy()) || (nullptr == m_pStrategyEngine))
	{
		net::SendError(id, req, InvalidRequest, "invalid strategy request");
		return false;
	}
	return m_pStrategyEngine->HandleStrategyRequest(req);
}

int CBrokerService::OnNetEvent(const net::CNetEvent& ev)
{
	if (net::em_event::request == ev.m_event)
	{
		OnClientRequest(ev.m_request->GetConnectionId(), *ev.m_request);
	}
	else if ((net::em_event::disconnected == ev.m_event) || (net::em_event::error == ev.m_event) || (net::em_event::timeout == ev.m_event))
	{
		HandleDisconnected(ev.m_connection_id);
	}
	return 1;
}

bool CBrokerService::HandleAccount(net::_TyConnectionId id, const CRequest& req)
{
	if (!IsAuthenticated(id) || (nullptr == m_pStrategyEngine))
	{
		net::SendError(id, req, AuthenticationRequired, "authentication required");
		return false;
	}
	std::string actor;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto mIter = m_clientUsers.find(id);
		if (m_clientUsers.end() != mIter)
		{
			actor = mIter->second;
		}
	}
	return m_pStrategyEngine->HandleAccountRequest(req, actor);
}

bool CBrokerService::HandleTrading(net::_TyConnectionId id, const CRequest& req)
{
	if (!IsAuthenticated(id) || nullptr == m_pStrategyEngine)
	{
		net::SendError(id, req, AuthenticationRequired, "authentication required");
		return false;
	}
	return m_pStrategyEngine->HandleTradingRequest(req);
}

CBrokerService::~CBrokerService()
{
	if (nullptr != m_pStrategyEngine)
	{
		m_pStrategyEngine->StopTrading();
	}
}
