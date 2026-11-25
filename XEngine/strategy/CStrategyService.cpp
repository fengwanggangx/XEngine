#include "../log/CLogger.h"
#include "TradeRequestRouting.h"
#include "CStrategyService.h"

#include "../common/defines.h"
#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../network/CNetTools.h"
#include "../request/RequestCenter.h"
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../system/CSession.h"
#include "../system/CHQService.h"
#include "CStrategyContext.h"
#include "CTradeService.h"
#include "../trade/TradeProtocol.h"
#include "../common/container.h"
#include "strategies/CDualMaCrossStrategy.h"
#include "strategies/CTakeProfitStopLossStrategy.h"
#include "strategies/CTrailingTakeProfitStrategy.h"
#include "strategies/CPriceBreakoutStrategy.h"
#include "../trade/OrderRules.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <set>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <utility>

namespace
{
	thread_local _TyStrategyId currentStrategyId{ 0 };

	CQuoteInfo MakeQuoteInfo(const hqmarket::market::v1::Security& security, Channel channel)
	{
		return CQuoteInfo(security.symbol(), static_cast<Exchange>(static_cast<int>(security.exchange())), channel);
	}

	bool ParseParameters(const std::string& strJson, CStrategyConfig& cfg)
	{
		rapidjson::Document document;
		document.Parse(strJson.c_str());
		if (document.HasParseError() || !document.IsObject())
		{
			return false;
		}
		for (auto mIter = document.MemberBegin(); document.MemberEnd() != mIter; ++mIter)
		{
			if (!mIter->name.IsString())
			{
				return false;
			}
			std::string strValue;
			if (mIter->value.IsString())
			{
				strValue.assign(mIter->value.GetString(), mIter->value.GetStringLength());
			}
			else if (mIter->value.IsInt64())
			{
				strValue = std::to_string(mIter->value.GetInt64());
			}
			else if (mIter->value.IsUint64())
			{
				strValue = std::to_string(mIter->value.GetUint64());
			}
			else if (mIter->value.IsDouble())
			{
				strValue = std::to_string(mIter->value.GetDouble());
			}
			else if (mIter->value.IsBool())
			{
				strValue = mIter->value.GetBool() ? "true" : "false";
			}
			else
			{
				return false;
			}
			cfg.m_params.emplace(std::string(mIter->name.GetString(), mIter->name.GetStringLength()), std::move(strValue));
		}
		return true;
	}

	bool ParseSubscriptions(const std::string& strJson, CStrategyConfig& cfg)
	{
		rapidjson::Document document;
		document.Parse(strJson.c_str());
		if (document.HasParseError() || !document.IsArray())
		{
			return false;
		}
		cfg.m_subscriptions.reserve(document.Size());
		for (const auto& item : document.GetArray())
		{
			if (!item.IsObject() || !item.HasMember("security") || !item["security"].IsString() || !item.HasMember("exchange") || !item["exchange"].IsString() || !item.HasMember("channel") || !item["channel"].IsString())
			{
				return false;
			}
			CQuoteInfo quote(item["security"].GetString(), ParseMarket(item["exchange"].GetString()), ParseChannel(item["channel"].GetString()));
			if (!quote.IsValid())
			{
				return false;
			}
			cfg.m_subscriptions.emplace_back(std::move(quote));
		}
		return !cfg.m_subscriptions.empty();
	}

	using _TySecuritySet = std::set<std::pair<std::string, std::string>>;

	_TySecuritySet BoundSecurities(const request::StrategyInfo& info)
	{
		_TySecuritySet securities;
		for (const auto& subscription : info.subscriptions())
		{
			securities.emplace(subscription.exchange(), subscription.security());
		}
		return securities;
	}

	bool MakeStrategyConfig(const request::StrategyInfo& info, CStrategyConfig& cfg)
	{
		cfg.m_id = info.strategy_id();
		cfg.m_account = FmtAccount(info);
		cfg.m_type = info.strategy_type();
		cfg.m_revision = info.config_revision();
		cfg.m_name = info.strategy_name();
		cfg.m_event_queue_limit = (0 == info.event_queue_limit()) ? 4096 : info.event_queue_limit();
		cfg.m_auto_start = info.auto_start();
		for (const auto& [strKey, strValue] : info.parameters())
		{
			cfg.m_params.emplace(strKey, strValue);
		}
		for (const request::StrategySubscription& item : info.subscriptions())
		{
			CQuoteInfo quote(item.security(), ParseMarket(item.exchange()), ParseChannel(item.channel()));
			if (!quote.IsValid())
			{
				return false;
			}
			cfg.m_subscriptions.emplace_back(std::move(quote));
		}
		std::string error;
		return cfg.IsValid() && (1000000 >= cfg.m_event_queue_limit) && (!info.auto_start() || info.enabled()) && CConfiguredStrategy::ValidateParameters(cfg, error);
	}

	std::string SerializeParameters(const request::StrategyInfo& info)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		for (const auto& [strKey, strValue] : info.parameters())
		{
			writer.Key(strKey.c_str(), static_cast<rapidjson::SizeType>(strKey.size()));
			writer.String(strValue.c_str(), static_cast<rapidjson::SizeType>(strValue.size()));
		}
		writer.EndObject();
		return std::string(buffer.GetString(), buffer.GetSize());
	}

	std::string SerializeSubscriptions(const request::StrategyInfo& info)
	{
		rapidjson::StringBuffer buffer;
		rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
		writer.StartArray();
		for (const request::StrategySubscription& item : info.subscriptions())
		{
			writer.StartObject();
			writer.Key("security");
			writer.String(item.security().c_str());
			writer.Key("exchange");
			writer.String(item.exchange().c_str());
			writer.Key("channel");
			writer.String(item.channel().c_str());
			writer.EndObject();
		}
		writer.EndArray();
		return std::string(buffer.GetString(), buffer.GetSize());
	}

	bool FillStrategyInfo(const db::_TyRow& values, request::StrategyInfo& info)
	{
		std::vector<std::string> row;
		row.reserve(values.size());
		for (const db::_TyCell& value : values)
		{
			row.emplace_back(db::to_string(value));
		}
		std::uint64_t id = 0;
		std::uint64_t queueLimit = 0;
		std::uint64_t revision = 0;
		CStrategyConfig cfg;
		if ((10 != row.size()) || !utility::to_number(row[9], revision) || (0 == revision) || !utility::to_number(row[0], id) || !utility::to_number(row[5], queueLimit) || !ParseParameters(row[3], cfg) || !ParseSubscriptions(row[4], cfg))
		{
			return false;
		}
		if (!DecodeAccount(row[8], cfg.m_account))
		{
			return false;
		}
		SetAccountIdentity(cfg.m_account, info);
		info.set_strategy_id(id);
		info.set_config_revision(revision);
		info.set_strategy_type(row[1]);
		info.set_strategy_name(row[2]);
		info.set_event_queue_limit(static_cast<std::uint32_t>(queueLimit));
		info.set_auto_start("1" == row[6]);
		info.set_enabled("1" == row[7]);
		for (const auto& [strKey, strValue] : cfg.m_params)
		{
			(*info.mutable_parameters())[strKey] = strValue;
		}
		for (const CQuoteInfo& quote : cfg.m_subscriptions)
		{
			request::StrategySubscription* pItem = info.add_subscriptions();
			pItem->set_security(quote.m_security.m_code);
			pItem->set_exchange(GetMarketString(quote.m_security.m_market));
			pItem->set_channel(GetChannelString(quote.m_channel));
		}
		return true;
	}

	bool SendStrategyResponse(const CRequest& req, const std::string& strMessage)
	{
		CRequest response;
		response.SetId(req.GetId());
		response.SetType(req.GetType());

		response.SetExtraData("message", strMessage);
		return net::SendRequest(req.GetConnectionId(), response);
	}
} // namespace

bool CStrategyEvent::IsMarketEvent() const
{
	return (StrategyEventType::Quote == m_type) || (StrategyEventType::Depth == m_type) || (StrategyEventType::MarketTrade == m_type) || (StrategyEventType::Bar == m_type);
}

CStrategyEvent::CStrategyEvent()
{
	static std::atomic_uint64_t s_id{ 1 };
	m_sequence = s_id.fetch_add(1, std::memory_order_relaxed);
}

bool CSignalKey::operator==(const CSignalKey& arg) const
{
	return (m_id == arg.m_id) && (m_signal_id == arg.m_signal_id);
}

std::size_t CSignalKeyHash::operator()(const CSignalKey& key) const
{
	std::size_t strategyHash = std::hash<_TyStrategyId>{}(key.m_id);
	std::size_t signalHash = std::hash<_TySignalId>{}(key.m_signal_id);
	return strategyHash ^ (signalHash + 0x9e3779b9U + (strategyHash << 6) + (strategyHash >> 2));
}

std::size_t CStrategyService::COrderRouteHash::operator()(const _TyOrderRouteIndex& key) const noexcept
{
	std::size_t accountHash = CAccountHash{}(key.first);
	std::size_t orderHash = std::hash<_TyOrderId>{}(key.second);
	return accountHash ^ (orderHash + 0x9e3779b9U + (accountHash << 6) + (accountHash >> 2));
}

CStrategyService::CStrategyService(CHQService* pHQService, CTradeService* pTradeService, CAccountService* pAccountService, CIndicator& indicator) : m_indicator(indicator), m_hq_service(pHQService), m_trade_service(pTradeService), m_account_service(pAccountService)
{
}

CStrategyService::~CStrategyService()
{
	StopAll();
	if (nullptr != m_trade_service)
	{
		m_trade_service->SetStrategyReplyHandler({});
		m_trade_service->SetEventHandler({});
		m_trade_service->SetAccountReadyHandler({});
	}
}

bool CStrategyService::Initialize()
{
	m_last_error.clear();
	if (!m_execution.Open())
	{
		m_last_error = m_execution.GetLastError();
		return false;
	}
	if (nullptr == m_hq_service || nullptr == m_trade_service || nullptr == m_account_service)
	{
		m_last_error = "Strategy market or trade service is unavailable";
		return false;
	}
	m_trade_service->SetStrategyReplyHandler(std::bind_front(static_cast<void (CStrategyService::*)(const CRequest&, const trade::CCounterReply&)>(&CStrategyService::OnReply), this));
	m_trade_service->SetEventHandler(std::bind_front(static_cast<void (CStrategyService::*)(const trade::CCounterEvent&)>(&CStrategyService::OnTradeEvent), this));
	m_trade_service->SetAccountReadyHandler(std::bind_front(&CStrategyService::OnAccountReady, this));
	return LoadStrategy();
}

bool CStrategyService::BeginAccountUse(const _TyAccount& account, _TyStrategyId strategyId)
{
	std::lock_guard<std::mutex> lock(m_account_users_mutex);
	std::unordered_set<_TyStrategyId>& strategies = m_running_strategies[account];
	if (container::with(strategies, strategyId) || !m_trade_service->IsReady(account) || !m_account_service->AcquireAccount(account))
	{
		return false;
	}
	strategies.emplace(strategyId);
	return true;
}

void CStrategyService::EndAccountUse(const _TyAccount& account, _TyStrategyId strategyId)
{
	std::lock_guard<std::mutex> lock(m_account_users_mutex);
	const auto mIter = m_running_strategies.find(account);
	if (m_running_strategies.end() != mIter && 0 != mIter->second.erase(strategyId))
	{
		m_account_service->ReleaseAccount(account);
		if (mIter->second.empty())
		{
			m_running_strategies.erase(mIter);
		}
	}
}

void CStrategyService::OnReply(const CRequest& origin, const trade::CCounterReply& reply)
{
	std::uint64_t strategyId = 0;
	std::uint64_t instanceId = 0;
	if (!tradingroute::ReadNumber(origin, "strategy_id", strategyId) || !tradingroute::ReadNumber(origin, "strategy_instance_id", instanceId))
	{
		CLogger::Write(Level::Error, "strategy", "Invalid strategy reply routing metadata");
		return;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(strategyId);
	if (nullptr == runtime || runtime->m_removing.load() || instanceId != runtime->m_instance_id || reply.m_account != runtime->m_config.m_account)
	{
		return;
	}
	if (reply.m_result.Ok())
	{
		return;
	}
	if ((trade::Operation::Submit == reply.m_operation) && ((trade::Error::UnknownResult == reply.m_result.m_error) || (trade::Error::Timeout == reply.m_result.m_error) || (trade::Error::Unavailable == reply.m_result.m_error)))
	{
		std::uint64_t client = 0;
		if (tradingroute::ReadNumber(origin, "client_order_id", client))
		{
			m_execution.MarkSubmission(client, false);
		}
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_metrics_mutex);
		runtime->m_last_error = reply.m_result.m_message;
	}
	NotifySnapshot(runtime);
	if (trade::Operation::Submit != reply.m_operation || (trade::Error::Rejected != reply.m_result.m_error && trade::Error::Unsupported != reply.m_result.m_error && trade::Error::Conflict != reply.m_result.m_error && trade::Error::NotFound != reply.m_result.m_error))
	{
		return;
	}
	std::uint64_t rejectedClient = 0;
	if (tradingroute::ReadNumber(origin, "client_order_id", rejectedClient))
	{
		m_execution.RejectBeforeAcceptance(rejectedClient, reply.m_result.m_message);
	}
	COrderEvent event;
	event.m_account = reply.m_account;
	event.m_id = strategyId;
	event.m_status = OrderStatus::Rejected;
	event.m_reason = reply.m_result.m_message;
	std::uint64_t quantity = 0;
	if (!tradingroute::ReadNumber(origin, "signal_id", event.m_signal_id) || !tradingroute::ReadNumber(origin, "client_order_id", event.m_client_order_id) || !tradingroute::ReadNumber(origin, "quantity", quantity) || static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) < quantity)
	{
		CLogger::Write(Level::Error, "strategy", "Invalid rejected strategy order metadata");
		return;
	}
	event.m_quantity = static_cast<std::int64_t>(quantity);
	std::string security = origin.GetExtraData("security");
	std::size_t dot = security.find('.');
	if (std::string::npos != dot)
	{
		event.m_security = CSecurity(security.substr(0, dot), ParseMarket(security.substr(dot + 1)));
	}
	{
		std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
		const auto mIter = m_runtimes.find(strategyId);
		if (m_runtimes.end() == mIter || mIter->second != runtime || runtime->m_removing.load())
		{
			return;
		}
		std::lock_guard<std::mutex> signalLock(m_signals_mutex);
		if (0 != event.m_signal_id)
		{
			m_signals[{ strategyId, event.m_signal_id }] = SignalState::Rejected;
		}
	}
	CStrategyEvent strategyEvent;
	strategyEvent.m_type = StrategyEventType::Order;
	strategyEvent.m_data = std::move(event);
	EnqueueEvent(runtime, std::move(strategyEvent));
}

void CStrategyService::OnTradeEvent(const trade::CCounterEvent& event)
{
	m_execution.Reconcile(event.m_account, m_trade_service->GetStrategyAccountView(event.m_account, {}));
	std::vector<std::shared_ptr<CStrategyRuntime>> affected;
	{
		std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
		for (const auto& [id, runtime] : m_runtimes)
		{
			if (event.m_account == runtime->m_config.m_account)
			{
				affected.emplace_back(runtime);
			}
		}
	}
	for (const auto& runtime : affected)
	{
		CStrategyEvent tick;
		tick.m_type = StrategyEventType::Timer;
		CTimerEvent timer;
		timer.m_zero_position = (trade::EventTy::Position == event.m_kind) && (0 == event.m_position.m_total) && !event.m_position.m_security.empty();
		if (trade::EventTy::Position == event.m_kind)
		{
			tick.m_security = event.m_position.m_security;
		}
		tick.m_data = timer;
		EnqueueEvent(runtime, std::move(tick));
	}

	if (trade::EventTy::Order == event.m_kind)
	{
		OnOrderEvent(trade::protocol::OrderEvent(event));
	}
	else if (trade::EventTy::Deal == event.m_kind)
	{
		OnTradeEvent(trade::protocol::TradeEvent(event));
	}
}

void CStrategyService::OnAccountReady(const _TyAccount& account)
{
	m_execution.Reconcile(account, m_trade_service->GetStrategyAccountView(account, {}));
	if (m_stopping.load())
	{
		return;
	}
	std::vector<_TyStrategyId> deferred;
	{
		std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
		for (const auto& item : m_runtimes)
		{
			if (account == item.second->m_config.m_account && item.second->m_config.m_auto_start && StrategyState::Initialized == item.second->m_state.load())
			{
				deferred.emplace_back(item.first);
			}
		}
	}
	for (_TyStrategyId id : deferred)
	{
		StartStrategy(id);
	}
}

bool CStrategyService::OnRequest(const CRequest& req)
{
	std::lock_guard<std::mutex> operationLock(m_operations_mutex);
	if (!req.IsStrategy())
	{
		net::SendError(req.GetConnectionId(), req, 1101, "invalid strategy request type");
		return false;
	}
	_TyRequest type = req.GetType();
	if (_TyRequest::STRATEGY_ACTION == type)
	{
		return ActionStrategy(req);
	}
	if (_TyRequest::STRATEGY_ADD == type)
	{
		return AddStrategy(req);
	}
	if (_TyRequest::STRATEGY_QUERY == type)
	{
		return QueryStrategies(req);
	}
	if (_TyRequest::STRATEGY_MODIFY == type)
	{
		return ModifyStrategy(req);
	}
	if (_TyRequest::STRATEGY_DELETE == type)
	{
		return DeleteStrategy(req);
	}
	if (_TyRequest::STRATEGY_RUNTIME_QUERY == type)
	{
		return QueryRuntime(req);
	}
	if ((_TyRequest::STRATEGY_START == type) || (_TyRequest::STRATEGY_PAUSE == type) || (_TyRequest::STRATEGY_STOP == type))
	{
		return ControlStrategy(req);
	}
	net::SendError(req.GetConnectionId(), req, 1102, "unsupported strategy type");
	return false;
}

bool CStrategyService::AddStrategy(const CRequest& req)
{
	if (!req.GetData().has_strategy())
	{
		net::SendError(req.GetConnectionId(), req, 1103, "strategy protobuf payload is required");
		return false;
	}
	request::StrategyInfo info = req.GetData().strategy();
	if (!IsAccountValid(FmtAccount(info)))
	{
		net::SendError(req.GetConnectionId(), req, 1202, "Account identity required");
		return false;
	}
	if (!m_account_service->HasAccount(FmtAccount(info)))
	{
		net::SendError(req.GetConnectionId(), req, 1114, "Trading account does not exist");
		return false;
	}
	info.set_strategy_id(1);
	info.set_config_revision(1);
	if (0 == info.event_queue_limit())
	{
		info.set_event_queue_limit(4096);
	}
	CStrategyConfig cfg;
	if (!MakeStrategyConfig(info, cfg) || (nullptr == std::unique_ptr<IStrategy>(CreateStrategy(info.strategy_type()))))
	{
		net::SendError(req.GetConnectionId(), req, 1103, "invalid strategy configuration");
		return false;
	}
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		net::SendError(req.GetConnectionId(), req, 1104, "strategy database is unavailable");
		return false;
	}
	std::string strSQL = "INSERT INTO table_strategy(strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,enabled,account_identity) VALUES(" + utility::Utf8Literal(info.strategy_type()) + "," + utility::Utf8Literal(info.strategy_name()) + "," + utility::Utf8Literal(SerializeParameters(info)) + "," + utility::Utf8Literal(SerializeSubscriptions(info)) + "," + std::to_string(info.event_queue_limit()) + "," + (info.auto_start() ? "1" : "0") + "," + (info.enabled() ? "1" : "0") + "," + utility::Utf8Literal(EncodeAccount(FmtAccount(info))) + ")";
	if (0 != pDB->ExecUpdate(strSQL))
	{
		net::SendError(req.GetConnectionId(), req, 1105, "failed to add strategy");
		return false;
	}
	const db::CTable& idTable = pDB->ExecQuery("SELECT LAST_INSERT_ID()");
	std::uint64_t id = 0;
	if (idTable.m_rows.empty() || idTable.m_rows.front().empty() || !utility::to_number(db::to_string(idTable.m_rows.front().front()), id))
	{
		net::SendError(req.GetConnectionId(), req, 1106, "failed to obtain strategy id");
		return false;
	}
	cfg.m_id = id;
	if (info.enabled() && !CreateStrategy(cfg))
	{
		pDB->ExecUpdate("DELETE FROM table_strategy WHERE strategy_id=" + std::to_string(id));
		net::SendError(req.GetConnectionId(), req, 1107, "failed to start strategy runtime");
		return false;
	}
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());

	info.set_strategy_id(id);
	response.SetData(info);
	return net::SendRequest(req.GetConnectionId(), response);
}

bool CStrategyService::ModifyStrategy(const CRequest& req)
{
	if (!req.GetData().has_strategy() || (0 == req.GetData().strategy().strategy_id()))
	{
		net::SendError(req.GetConnectionId(), req, 1103, "valid strategy protobuf payload is required");
		return false;
	}
	request::StrategyInfo info = req.GetData().strategy();
	if (!IsAccountValid(FmtAccount(info)))
	{
		db::_TyDBPtr lookup = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
		if (nullptr == lookup)
		{
			net::SendError(req.GetConnectionId(), req, 1104, "strategy database is unavailable");
			return false;
		}
		const db::CTable& accountRows = lookup->ExecQuery("SELECT account_identity FROM table_strategy WHERE strategy_id=" + std::to_string(info.strategy_id()));
		_TyAccount account;
		if (accountRows.m_rows.empty() || accountRows.m_rows.front().empty() || !DecodeAccount(db::to_string(accountRows.m_rows.front().front()), account))
		{
			net::SendError(req.GetConnectionId(), req, 1110, "strategy does not exist");
			return false;
		}
		SetAccountIdentity(account, info);
	}
	if (!m_account_service->HasAccount(FmtAccount(info)))
	{
		net::SendError(req.GetConnectionId(), req, 1114, "Trading account does not exist");
		return false;
	}
	if (0 == info.event_queue_limit())
	{
		info.set_event_queue_limit(4096);
	}
	CStrategyConfig cfg;
	if (!MakeStrategyConfig(info, cfg) || (nullptr == std::unique_ptr<IStrategy>(CreateStrategy(info.strategy_type()))))
	{
		net::SendError(req.GetConnectionId(), req, 1103, "invalid strategy configuration");
		return false;
	}
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		net::SendError(req.GetConnectionId(), req, 1104, "strategy database is unavailable");
		return false;
	}
	const db::CTable& existing = pDB->ExecQuery("SELECT strategy_id,strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,enabled,account_identity,config_revision FROM table_strategy WHERE strategy_id=" + std::to_string(cfg.m_id));
	if (existing.m_rows.empty())
	{
		net::SendError(req.GetConnectionId(), req, 1110, "strategy does not exist");
		return false;
	}
	request::StrategyInfo previous;
	if (!FillStrategyInfo(existing.m_rows.front(), previous) || (0 == previous.subscriptions_size()) || (0 == info.config_revision()) || (previous.config_revision() != info.config_revision()) || (info.config_revision() >= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) || (previous.strategy_type() != info.strategy_type()) || (FmtAccount(previous) != FmtAccount(info)) || (BoundSecurities(previous) != BoundSecurities(info)) || m_execution.HasUnresolved(cfg.m_id))
	{
		net::SendError(req.GetConnectionId(), req, 1112, "Configuration revision changed, immutable binding changed, or execution remains unresolved");
		return false;
	}
	std::shared_ptr<CStrategyRuntime> oldRuntime = FindRuntime(cfg.m_id);
	CStrategyConfig oldConfig;
	if (nullptr != oldRuntime)
	{
		oldConfig = oldRuntime->m_config;
		StrategyState oldState = oldRuntime->m_state.load();
		bool ownActiveOrders = false;
		{
			std::lock_guard<std::mutex> lock(oldRuntime->m_orders_mutex);
			ownActiveOrders = !oldRuntime->m_active_order_ids.empty();
		}
		if (((StrategyState::Initialized != oldState) && (StrategyState::Stopped != oldState)) || ownActiveOrders)
		{
			net::SendError(req.GetConnectionId(), req, 1115, "Stop strategy and resolve orders before modifying configuration");
			return false;
		}
		if (!StopStrategy(cfg.m_id) || !RemoveStrategy(cfg.m_id))
		{
			net::SendError(req.GetConnectionId(), req, 1108, "strategy runtime cannot be replaced");
			return false;
		}
	}
	CStrategyConfig staged = cfg;
	staged.m_auto_start = false;
	if (info.enabled() && !CreateStrategy(staged))
	{
		if (nullptr != oldRuntime)
		{
			CreateStrategy(oldConfig);
		}
		net::SendError(req.GetConnectionId(), req, 1107, "failed to create strategy runtime");
		return false;
	}
	std::string strSQL = "UPDATE table_strategy SET config_revision=config_revision+1,strategy_type=" + utility::Utf8Literal(info.strategy_type()) + ",strategy_name=" + utility::Utf8Literal(info.strategy_name()) + ",parameters=" + utility::Utf8Literal(SerializeParameters(info)) + ",subscriptions=" + utility::Utf8Literal(SerializeSubscriptions(info)) + ",event_queue_limit=" + std::to_string(info.event_queue_limit()) + ",auto_start=" + (info.auto_start() ? "1" : "0") + ",enabled=" + (info.enabled() ? "1" : "0") + ",account_identity=" + utility::Utf8Literal(EncodeAccount(FmtAccount(info))) + " WHERE strategy_id=" + std::to_string(cfg.m_id) + " AND config_revision=" + std::to_string(info.config_revision());
	bool updated = 0 == pDB->ExecUpdate(strSQL);
	if (updated)
	{
		const db::CTable& affected = pDB->ExecQuery("SELECT ROW_COUNT()");
		std::uint64_t count = 0;
		updated = !affected.m_rows.empty() && !affected.m_rows.front().empty() && utility::to_number(db::to_string(affected.m_rows.front().front()), count) && (1 == count);
	}
	if (!updated)
	{
		if (info.enabled())
		{
			StopStrategy(cfg.m_id);
			RemoveStrategy(cfg.m_id);
		}
		if (nullptr != oldRuntime)
		{
			CreateStrategy(oldConfig);
		}
		net::SendError(req.GetConnectionId(), req, 1111, "failed to modify strategy");
		return false;
	}
	info.set_config_revision(info.config_revision() + 1);
	{
		std::unique_lock<std::shared_mutex> lock(m_strategies_mutex);
		const auto mIter = m_runtimes.find(cfg.m_id);
		if (m_runtimes.end() != mIter)
		{
			mIter->second->m_config.m_revision = info.config_revision();
			mIter->second->m_config.m_auto_start = cfg.m_auto_start;
		}
	}
	if (info.enabled() && cfg.m_auto_start && !StartStrategy(cfg.m_id))
	{
		m_last_error = "Configuration saved; automatic start requires attention";
	}
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());

	response.SetData(info);
	return net::SendRequest(req.GetConnectionId(), response);
}

bool CStrategyService::QueryStrategies(const CRequest& req) const
{
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		net::SendError(req.GetConnectionId(), req, 1104, "strategy database is unavailable");
		return false;
	}
	const db::CTable& table = pDB->ExecQuery("SELECT strategy_id,strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,enabled,account_identity,config_revision FROM table_strategy ORDER BY strategy_id");
	request::StrategyList strategies;
	for (const db::_TyRow& row : table.m_rows)
	{
		request::StrategyInfo* pInfo = strategies.add_strategies();
		if (!FillStrategyInfo(row, *pInfo))
		{
			strategies.mutable_strategies()->RemoveLast();
		}
	}
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());

	response.SetData(strategies);
	return net::SendRequest(req.GetConnectionId(), response);
}

bool CStrategyService::DeleteStrategy(const CRequest& req)
{
	std::uint64_t id = 0;
	if (!utility::to_number(req.GetExtraData("strategy_id"), id) || (0 == id))
	{
		net::SendError(req.GetConnectionId(), req, 1103, "invalid strategy id");
		return false;
	}
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		net::SendError(req.GetConnectionId(), req, 1104, "strategy database is unavailable");
		return false;
	}
	const db::CTable& existing = pDB->ExecQuery("SELECT strategy_id FROM table_strategy WHERE strategy_id=" + std::to_string(id));
	if (existing.m_rows.empty())
	{
		net::SendError(req.GetConnectionId(), req, 1110, "strategy does not exist");
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	CStrategyConfig oldConfig;
	StrategyState oldState = StrategyState::Created;
	if (nullptr != runtime)
	{
		oldConfig = runtime->m_config;
		oldState = runtime->m_state.load();
		if (!StopStrategy(id) || !RemoveStrategy(id))
		{
			net::SendError(req.GetConnectionId(), req, 1108, "strategy runtime cannot be removed");
			return false;
		}
	}
	if (0 != pDB->ExecUpdate("DELETE FROM table_strategy WHERE strategy_id=" + std::to_string(id)))
	{
		if (nullptr != runtime)
		{
			if (CreateStrategy(oldConfig) && !oldConfig.m_auto_start && ((StrategyState::Running == oldState) || (StrategyState::Starting == oldState)))
			{
				StartStrategy(id);
			}
		}
		net::SendError(req.GetConnectionId(), req, 1109, "failed to delete strategy");
		return false;
	}
	return SendStrategyResponse(req, "strategy deleted");
}

bool CStrategyService::QueryRuntime(const CRequest& req) const
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());

	std::vector<CStrategySnapshot> snapshots = GetSnapshots();
	if (10000 < snapshots.size())
	{
		net::SendError(req.GetConnectionId(), req, 1113, "too many strategy runtimes");
		return false;
	}
	request::StrategyRuntimeList list;
	for (const auto& snapshot : snapshots)
	{
		request::StrategyRuntimeRecord* pRecord = list.add_runtimes();
		pRecord->set_strategy_id(snapshot.m_id);
		pRecord->set_state(static_cast<int>(snapshot.m_state));
		pRecord->set_market_available(snapshot.m_market_available);
		pRecord->set_queue_length(snapshot.m_queue_length);
		pRecord->set_active_orders(snapshot.m_active_order_count);
		pRecord->set_last_error(snapshot.m_last_error);
		pRecord->set_business_state(snapshot.m_business_state);
		pRecord->set_details_json(snapshot.m_details_json);
	}
	response.SetData(list);
	return net::SendRequest(req.GetConnectionId(), response);
}

bool CStrategyService::ControlStrategy(const CRequest& req)
{
	std::uint64_t id = 0;
	if (!utility::to_number(req.GetExtraData("strategy_id"), id) || (0 == id))
	{
		net::SendError(req.GetConnectionId(), req, 1103, "invalid strategy id");
		return false;
	}
	if (!GetSnapshot(id).has_value())
	{
		net::SendError(req.GetConnectionId(), req, 1110, "enabled strategy runtime does not exist");
		return false;
	}
	_TyRequest type = req.GetType();
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((_TyRequest::STRATEGY_START == type) && (nullptr != runtime) && !m_trade_service->IsReady(runtime->m_config.m_account))
	{
		net::SendError(req.GetConnectionId(), req, 1116, "交易账户未连接，请先连接账户");
		return false;
	}
	bool success = (_TyRequest::STRATEGY_START == type) ? StartStrategy(id) : ((_TyRequest::STRATEGY_PAUSE == type) ? PauseStrategy(id) : StopStrategy(id));
	if (!success)
	{
		net::SendError(req.GetConnectionId(), req, 1112, "strategy state does not allow this operation");
		return false;
	}
	return SendStrategyResponse(req, "strategy state updated");
}

const std::string& CStrategyService::GetLastError() const
{
	return m_last_error;
}

bool CStrategyService::LoadStrategy()
{
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		m_last_error = "Strategy database is unavailable";
		return false;
	}
	const db::CTable& test = pDB->ExecQuery("SHOW TABLES LIKE 'table_strategy'");
	if (test.m_rows.empty())
	{
		m_last_error = "MySQL table_strategy does not exist";
		return false;
	}
	const db::CTable& table = pDB->ExecQuery("SELECT strategy_id,strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,account_identity,config_revision FROM table_strategy WHERE enabled=1 ORDER BY strategy_id");

	std::vector<std::string> row;
	for (const db::_TyRow& item : table.m_rows)
	{
		if (9 != db::to_string_row(item, row))
		{
			m_last_error = "table_strategy contains an invalid row";
			return false;
		}
		CStrategyConfig cfg;
		std::uint64_t id = 0;
		std::uint64_t eventQueueLimit = 0;
		std::uint64_t autoStart = 0;
		if (!utility::to_number(row[0], id) || !utility::to_number(row[5], eventQueueLimit) || !utility::to_number(row[6], autoStart))
		{
			m_last_error = "table_strategy contains invalid numeric fields";
			return false;
		}
		cfg.m_id = id;
		if (!utility::to_number(row[8], cfg.m_revision) || (0 == cfg.m_revision))
		{
			return false;
		}
		cfg.m_type = row[1];
		cfg.m_name = row[2];
		cfg.m_event_queue_limit = static_cast<std::size_t>(eventQueueLimit);
		cfg.m_auto_start = 0 != autoStart;
		if (!DecodeAccount(row[7], cfg.m_account))
		{
			m_last_error = "Invalid strategy trading account";
			return false;
		}
		if (!ParseParameters(row[3], cfg) || !ParseSubscriptions(row[4], cfg) || !CreateStrategy(cfg))
		{
			m_last_error = "Failed to load strategy " + row[0] + " from table_strategy";
			return false;
		}
	}
	return true;
}

IStrategy* CStrategyService::CreateStrategy(const std::string& strType) const
{
	if ("dual_ma_cross" == strType)
	{
		return new CDualMaCrossStrategy(m_indicator);
	}
	if ("take_profit_stop_loss" == strType)
	{
		return new CTakeProfitStopLossStrategy();
	}
	if ("trailing_take_profit" == strType)
	{
		return new CTrailingTakeProfitStrategy();
	}
	if ("price_breakout" == strType)
	{
		return new CPriceBreakoutStrategy();
	}
	return nullptr;
}

bool CStrategyService::CreateStrategy(const CStrategyConfig& cfg)
{
	if (!cfg.IsValid() || m_stopping.load())
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = std::make_shared<CStrategyRuntime>();
	runtime->m_instance_id = m_instance_id.fetch_add(1);
	runtime->m_config = cfg;
	std::unordered_map<std::string, std::vector<CQuoteInfo>> subscriptions;
	subscriptions.reserve(cfg.m_subscriptions.size());
	for (const auto& item : cfg.m_subscriptions)
	{
		subscriptions[item.m_security.String()].push_back(item);
	}
	try
	{
		bool bMulStocks = subscriptions.size() > 1;
		for (auto& [k, v] : subscriptions)
		{
			CStrategyStockInstance& instance = runtime->m_securities.emplace_back();
			instance.m_security = v.front().m_security;
			instance.m_strategy.reset(CreateStrategy(cfg.m_type));
			instance.m_context = std::make_unique<CStrategyContext>(cfg.m_id, cfg.m_account, this, m_trade_service, instance.m_security, bMulStocks);
			CStrategyConfig config = cfg;
			config.m_subscriptions = std::move(v);
			if ((nullptr == instance.m_strategy) || !instance.m_strategy->Initialize(*instance.m_context, config))
			{
				return false;
			}
		}
	}
	catch (const std::exception& error)
	{
		m_last_error = error.what();
		return false;
	}
	runtime->m_state.store(StrategyState::Initialized);

	{
		std::unique_lock<std::shared_mutex> lock(m_strategies_mutex);
		if (!m_runtimes.emplace(cfg.m_id, runtime).second)
		{
			return false;
		}
	}

	std::vector<trade::COrder> orders = m_trade_service->GetOrders(cfg.m_account);
	for (const auto& item : orders)
	{
		if (cfg.m_id != item.m_id)
		{
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(m_signals_mutex);
			m_signals[{ cfg.m_id, item.m_signal_id }] = SignalState::Accepted;
		}
		if (trade::IsActive(item.m_status))
		{
			{
				std::lock_guard<std::mutex> lock(runtime->m_orders_mutex);
				runtime->m_active_order_ids.emplace(item.m_order_id);
			}
			{
				std::unique_lock<std::shared_mutex> lock(m_orders_mutex);
				m_order_routes[{ item.m_account, item.m_order_id }] = cfg.m_id;
				m_client_order_routes[{ item.m_account, item.m_client_id }] = cfg.m_id;
			}
		}
	}
	NotifySnapshot(runtime);
	if (cfg.m_auto_start && m_trade_service->IsReady(cfg.m_account) && !StartStrategy(cfg.m_id))
	{
		RemoveStrategy(cfg.m_id);
		return false;
	}
	return true;
}

bool CStrategyService::RemoveStrategy(_TyStrategyId id)
{
	if (m_execution.HasUnresolved(id))
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}
	StrategyState state = runtime->m_state.load();
	if ((StrategyState::Stopped != state) && (StrategyState::Initialized != state) && (StrategyState::Faulted != state))
	{
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_orders_mutex);
		if (!runtime->m_active_order_ids.empty())
		{
			return false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_events_mutex);
		if (runtime->m_removing.exchange(true))
		{
			return false;
		}
		runtime->m_accept_market_events.store(false);
	}
	RemoveRoutes(runtime->m_config);
	WaitUntilIdle(runtime);
	for (auto& stockInstance : runtime->m_securities)
	{
		stockInstance.m_context->Disable();
	}
	{
		std::unique_lock<std::shared_mutex> lock(m_strategies_mutex);
		m_runtimes.erase(id);
	}
	{
		std::lock_guard<std::mutex> lock(m_signals_mutex);
		std::erase_if(m_signals, [id](const auto& item)
		{
			return id == item.first.m_id;
		});
	}
	{
		std::unique_lock<std::shared_mutex> lock(m_orders_mutex);
		std::erase_if(m_order_routes, [id](const auto& item)
		{
			return id == item.second;
		});
		std::erase_if(m_client_order_routes, [id](const auto& item)
		{
			return id == item.second;
		});
	}
	return true;
}

bool CStrategyService::StartStrategy(_TyStrategyId id)
{
	if (m_stopping.load() || nullptr == m_trade_service || nullptr == m_account_service)
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}
	StrategyState state = runtime->m_state.load();
	if (((StrategyState::Initialized != state) && (StrategyState::Paused != state) && (StrategyState::Stopped != state)) || !BeginAccountUse(runtime->m_config.m_account, id))
	{
		return false;
	}
	bool bStateChanged = false;
	while ((StrategyState::Initialized == state) || (StrategyState::Paused == state) || (StrategyState::Stopped == state))
	{
		if (runtime->m_state.compare_exchange_weak(state, StrategyState::Starting))
		{
			bStateChanged = true;
			break;
		}
	}
	if (!bStateChanged)
	{
		EndAccountUse(runtime->m_config.m_account, id);
		return false;
	}
	if ((StrategyState::Initialized == state) || (StrategyState::Stopped == state))
	{
		AddRoutes(runtime->m_config);
	}
	if (!ExecuteControl(id, StrategyEventType::Start))
	{
		runtime->m_state.store(state);
		EndAccountUse(runtime->m_config.m_account, id);
		RemoveRoutes(runtime->m_config);
		return false;
	}
	return true;
}

bool CStrategyService::PauseStrategy(_TyStrategyId id)
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}
	StrategyState expected = StrategyState::Running;
	if (!runtime->m_state.compare_exchange_strong(expected, StrategyState::Pausing))
	{
		return false;
	}
	runtime->m_accept_market_events.store(false);
	if (!ExecuteControl(id, StrategyEventType::Pause))
	{
		runtime->m_state.store(StrategyState::Running);
		runtime->m_accept_market_events.store(true);
		return false;
	}
	EndAccountUse(runtime->m_config.m_account, id);
	return true;
}

bool CStrategyService::StopStrategy(_TyStrategyId id)
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}
	StrategyState state = runtime->m_state.load();
	if ((StrategyState::Stopped == state) || (StrategyState::Initialized == state))
	{
		return true;
	}
	while ((StrategyState::Stopping != state) && !runtime->m_state.compare_exchange_weak(state, StrategyState::Stopping))
	{
	}
	if (StrategyState::Stopping == state)
	{
		return false;
	}
	runtime->m_accept_market_events.store(false);
	bool bStopped = ExecuteControl(id, StrategyEventType::Stop);
	if (!bStopped && (StrategyState::Faulted != runtime->m_state.load()))
	{
		runtime->m_state.store(state);
	}
	RemoveRoutes(runtime->m_config);
	if (bStopped)
	{
		EndAccountUse(runtime->m_config.m_account, id);
	}
	return bStopped;
}

void CStrategyService::StopAll()
{
	if (m_stopping.exchange(true))
	{
		return;
	}
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
		runtimes.reserve(m_runtimes.size());
		for (const auto& item : m_runtimes)
		{
			runtimes.emplace_back(item.second);
		}
	}
	for (const auto& runtime : runtimes)
	{
		runtime->m_accept_market_events.store(false);
		StrategyState state = runtime->m_state.load();
		if ((StrategyState::Initialized != state) && (StrategyState::Stopped != state))
		{
			StopStrategy(runtime->m_config.m_id);
		}
		RemoveRoutes(runtime->m_config);
	}
	for (const auto& runtime : runtimes)
	{
		WaitUntilIdle(runtime);
		for (auto& stockInstance : runtime->m_securities)
		{
			stockInstance.m_context->Disable();
		}
		EndAccountUse(runtime->m_config.m_account, runtime->m_config.m_id);
	}
}

bool CStrategyService::OnReply(const CRequest& req)
{
	if (m_stopping.load() || !m_market_available.load() || (!req.IsMarket()))
	{
		return false;
	}
	std::string strKey = GetMarketKey(req);
	if (strKey.empty())
	{
		return false;
	}
	const _TyReqData& data = req.GetData();
	CStrategyEvent ev;
	if (data.has_quote())
	{
		ev.m_type = StrategyEventType::Quote;
		ev.m_data = data.quote();
		ev.m_security = MakeQuoteInfo(data.quote().security(), Channel::quote).m_security.String();
	}
	else if (data.has_depth())
	{
		ev.m_type = StrategyEventType::Depth;
		ev.m_data = data.depth();
		ev.m_security = MakeQuoteInfo(data.depth().security(), Channel::depth).m_security.String();
	}
	else if (data.has_trade())
	{
		ev.m_type = StrategyEventType::MarketTrade;
		ev.m_data = data.trade();
		ev.m_security = MakeQuoteInfo(data.trade().security(), Channel::trade).m_security.String();
	}
	else if (data.has_bar())
	{
		ev.m_type = StrategyEventType::Bar;
		ev.m_data = data.bar();
		ev.m_security = MakeQuoteInfo(data.bar().security(), Channel::bar_1d).m_security.String();
	}
	else
	{
		return false;
	}
	RouteMarketEvent(strKey, std::move(ev));
	return true;
}

void CStrategyService::OnHQMarketState(SessionState state, const std::string& strReason)
{
	bool bAvailable = SessionState::Ready == state;
	m_market_available.store(bAvailable);
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
		for (const auto& item : m_runtimes)
		{
			runtimes.emplace_back(item.second);
		}
	}
	for (const auto& runtime : runtimes)
	{
		CStrategyEvent stateEvent;
		stateEvent.m_type = StrategyEventType::MarketState;
		stateEvent.m_data = bAvailable;
		EnqueueEvent(runtime, std::move(stateEvent));
		if (!bAvailable)
		{
			std::lock_guard<std::mutex> lock(runtime->m_metrics_mutex);
			runtime->m_last_error = strReason;
		}
		NotifySnapshot(runtime);
	}
}

void CStrategyService::OnOrderEvent(const COrderEvent& ev)
{
	_TyStrategyId id = ev.m_id;
	if (0 == id)
	{
		std::shared_lock<std::shared_mutex> lock(m_orders_mutex);
		const auto mIter = m_order_routes.find({ ev.m_account, ev.m_order_id });
		if (m_order_routes.end() != mIter)
		{
			id = mIter->second;
		}
		else
		{
			const auto clientIter = m_client_order_routes.find({ ev.m_account, ev.m_client_order_id });
			if (m_client_order_routes.end() != clientIter)
			{
				id = clientIter->second;
			}
		}
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (ev.m_account != runtime->m_config.m_account))
	{
		return;
	}
	if (0 != ev.m_signal_id)
	{
		std::lock_guard<std::mutex> lock(m_signals_mutex);
		m_signals[{ id, ev.m_signal_id }] = OrderStatus::Rejected == ev.m_status ? SignalState::Rejected : SignalState::Accepted;
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_orders_mutex);
		if (IsFinishedOrderStatus(ev.m_status))
		{
			runtime->m_active_order_ids.erase(ev.m_order_id);
		}
		else if (0 != ev.m_order_id)
		{
			runtime->m_active_order_ids.emplace(ev.m_order_id);
		}
	}
	if (!IsFinishedOrderStatus(ev.m_status) && 0 != ev.m_order_id)
	{
		std::unique_lock<std::shared_mutex> lock(m_orders_mutex);
		m_order_routes[{ ev.m_account, ev.m_order_id }] = id;
		m_client_order_routes[{ ev.m_account, ev.m_client_order_id }] = id;
	}
	// Keep correlation through terminal order reports: a broker may deliver its fill afterward.

	CStrategyEvent strategyEvent;
	strategyEvent.m_type = StrategyEventType::Order;
	strategyEvent.m_data = ev;
	EnqueueEvent(runtime, std::move(strategyEvent));
}

void CStrategyService::OnTradeEvent(const CTradeEvent& ev)
{
	_TyStrategyId id = ev.m_id;
	if (0 == id)
	{
		std::shared_lock<std::shared_mutex> lock(m_orders_mutex);
		const auto mIter = m_order_routes.find({ ev.m_account, ev.m_order_id });
		if (m_order_routes.end() != mIter)
		{
			id = mIter->second;
		}
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (ev.m_account != runtime->m_config.m_account))
	{
		return;
	}
	CStrategyEvent strategyEvent;
	strategyEvent.m_type = StrategyEventType::Trade;
	strategyEvent.m_data = ev;
	EnqueueEvent(runtime, std::move(strategyEvent));
}

std::optional<CStrategySnapshot> CStrategyService::GetSnapshot(_TyStrategyId id) const
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return std::nullopt;
	}
	return MakeSnapshot(runtime);
}

std::vector<CStrategySnapshot> CStrategyService::GetSnapshots() const
{
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
		runtimes.reserve(m_runtimes.size());
		for (const auto& item : m_runtimes)
		{
			runtimes.emplace_back(item.second);
		}
	}
	std::vector<CStrategySnapshot> snapshots;
	snapshots.reserve(runtimes.size());
	for (const auto& runtime : runtimes)
	{
		snapshots.emplace_back(MakeSnapshot(runtime));
	}
	return snapshots;
}

void CStrategyService::RegisterStateHandler(_TyStateHandler&& handler)
{
	if (nullptr == handler)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_handlers_mutex);
	m_state_handlers.emplace_back(std::move(handler));
}

bool CStrategyService::IsMarketAvailable() const
{
	return m_market_available.load();
}

std::shared_ptr<CStrategyRuntime> CStrategyService::FindRuntime(_TyStrategyId id) const
{
	std::shared_lock<std::shared_mutex> lock(m_strategies_mutex);
	const auto mIter = m_runtimes.find(id);
	return m_runtimes.end() == mIter ? nullptr : mIter->second;
}

bool CStrategyService::ExecuteControl(_TyStrategyId id, StrategyEventType type)
{
	if (id == currentStrategyId)
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}
	CStrategyEvent ev;
	ev.m_type = type;
	ev.m_completion = std::make_shared<std::promise<bool>>();
	std::future<bool> completion = ev.m_completion->get_future();
	if (!EnqueueEvent(runtime, std::move(ev)))
	{
		return false;
	}
	return completion.get();
}

bool CStrategyService::EnqueueEvent(const std::shared_ptr<CStrategyRuntime>& runtime, CStrategyEvent&& ev)
{
	if (nullptr == runtime)
	{
		return false;
	}
	bool bSchedule = false;
	bool bFault = false;
	{
		std::lock_guard<std::mutex> lock(runtime->m_events_mutex);
		if (runtime->m_removing.load() || (ev.IsMarketEvent() && !runtime->m_accept_market_events.load()))
		{
			return false;
		}
		if (runtime->m_config.m_event_queue_limit <= runtime->m_events.size())
		{
			const auto vIter = std::find_if(runtime->m_events.begin(), runtime->m_events.end(), [](const CStrategyEvent& queuedEvent)
			{
				return queuedEvent.IsMarketEvent();
			});
			if (runtime->m_events.end() != vIter)
			{
				if (nullptr != vIter->m_completion)
				{
					vIter->m_completion->set_value(false);
				}
				runtime->m_events.erase(vIter);
				std::lock_guard<std::mutex> metricsLock(runtime->m_metrics_mutex);
				++runtime->m_dropped_market_events;
			}
			else if (ev.IsMarketEvent())
			{
				std::lock_guard<std::mutex> metricsLock(runtime->m_metrics_mutex);
				++runtime->m_dropped_market_events;
				return false;
			}
			else if ((runtime->m_config.m_event_queue_limit * 2) <= runtime->m_events.size())
			{
				bFault = true;
			}
		}
		if (!bFault)
		{
			runtime->m_events.emplace_back(std::move(ev));
		}
		if (!bFault && !runtime->m_scheduled)
		{
			runtime->m_scheduled = true;
			bSchedule = true;
		}
	}
	if (bFault)
	{
		HandleStrategyException(*runtime, "Strategy critical event queue exceeded hard limit");
		return false;
	}
	if (bSchedule)
	{
		ThreadPoolPtr->PushTask(task_priority::em_normal, 0, [this, runtime]()
		{
			DrainEvents(runtime);
		});
	}
	return true;
}

void CStrategyService::DrainEvents(const std::shared_ptr<CStrategyRuntime>& runtime)
{
	while (true)
	{
		CStrategyEvent event;
		{
			std::lock_guard<std::mutex> lock(runtime->m_events_mutex);
			if (runtime->m_events.empty())
			{
				runtime->m_scheduled = false;
				runtime->m_executing_callback = false;
				runtime->m_idle_condition.notify_all();
				return;
			}
			event = std::move(runtime->m_events.front());
			runtime->m_events.pop_front();
			runtime->m_executing_callback = true;
		}
		bool bResult = DispatchEvent(*runtime, event);
		if (nullptr != event.m_completion)
		{
			event.m_completion->set_value(bResult);
		}
		{
			std::lock_guard<std::mutex> lock(runtime->m_events_mutex);
			runtime->m_executing_callback = false;
		}
	}
}

bool CStrategyService::DispatchEvent(CStrategyRuntime& runtime, CStrategyEvent& event)
{
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	bool bResult = true;
	_TyStrategyId previousStrategyId = currentStrategyId;
	currentStrategyId = runtime.m_config.m_id;
	try
	{
		for (auto& instance : runtime.m_securities)
		{
			if (!event.m_security.empty() && (event.m_security != instance.m_security.String()))
			{
				continue;
			}
			switch (event.m_type)
			{
			case StrategyEventType::Action:
				bResult = instance.m_strategy->OnAction(event.m_action) && bResult;
				break;
			case StrategyEventType::History:
				instance.m_strategy->OnHistory(std::get<std::vector<_TyBarData>>(event.m_data));
				break;
			case StrategyEventType::MarketState:
				instance.m_strategy->OnMarketState(std::get<bool>(event.m_data));
				break;
			case StrategyEventType::Start:
				instance.m_strategy->OnStart();
				break;
			case StrategyEventType::Pause:
				instance.m_strategy->OnPause();
				break;
			case StrategyEventType::Stop:
				instance.m_strategy->OnStop();
				break;
			case StrategyEventType::Quote:
				instance.m_strategy->OnQuote(std::get<_TyQuoteData>(event.m_data));
				break;
			case StrategyEventType::Depth:
				instance.m_strategy->OnDepth(std::get<_TyDepthData>(event.m_data));
				break;
			case StrategyEventType::MarketTrade:
				instance.m_strategy->OnMarketTrade(std::get<_TyMarketTradeData>(event.m_data));
				break;
			case StrategyEventType::Bar:
				instance.m_strategy->OnBar(std::get<_TyBarData>(event.m_data));
				break;
			case StrategyEventType::Order:
				if (instance.m_security == std::get<COrderEvent>(event.m_data).m_security)
				{
					instance.m_strategy->OnOrder(std::get<COrderEvent>(event.m_data));
				}
				break;
			case StrategyEventType::Trade:
				if (instance.m_security == std::get<CTradeEvent>(event.m_data).m_security)
				{
					instance.m_strategy->OnTrade(std::get<CTradeEvent>(event.m_data));
				}
				break;
			case StrategyEventType::Timer:
				instance.m_strategy->OnTimer(std::get<CTimerEvent>(event.m_data));
				break;
			}
		}
		if (StrategyEventType::Start == event.m_type)
		{
			runtime.m_state.store(StrategyState::Running);
			runtime.m_accept_market_events.store(true);
		}
		else if (StrategyEventType::Pause == event.m_type)
		{
			runtime.m_state.store(StrategyState::Paused);
		}
		else if (StrategyEventType::Stop == event.m_type)
		{
			runtime.m_state.store(StrategyState::Stopped);
		}
	}
	catch (const std::exception& ex)
	{
		HandleStrategyException(runtime, ex.what());
		bResult = false;
	}
	catch (...)
	{
		HandleStrategyException(runtime, "Unknown strategy exception");
		bResult = false;
	}
	currentStrategyId = previousStrategyId;
	std::int64_t duration = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
	{
		std::lock_guard<std::mutex> lock(runtime.m_metrics_mutex);
		runtime.m_last_event_sequence = event.m_sequence;
		runtime.m_last_callback_duration_us = duration;
		runtime.m_max_callback_duration_us = (std::max)(runtime.m_max_callback_duration_us, duration);
	}
	std::shared_ptr<CStrategyRuntime> runtimeHandle = FindRuntime(runtime.m_config.m_id);
	if (nullptr != runtimeHandle)
	{
		NotifySnapshot(runtimeHandle);
	}
	return bResult;
}

void CStrategyService::HandleStrategyException(CStrategyRuntime& runtime, const std::string& strError)
{
	runtime.m_accept_market_events.store(false);
	runtime.m_state.store(StrategyState::Faulted);
	EndAccountUse(runtime.m_config.m_account, runtime.m_config.m_id);
	std::lock_guard<std::mutex> lock(runtime.m_metrics_mutex);
	runtime.m_last_error = strError;
}

void CStrategyService::RouteMarketEvent(const std::string& strKey, CStrategyEvent&& ev)
{
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> routesLock(m_routes_mutex);
		const auto routeIter = m_market_routes.find(strKey);
		if (m_market_routes.end() == routeIter)
		{
			return;
		}
		std::shared_lock<std::shared_mutex> strategiesLock(m_strategies_mutex);
		runtimes.reserve(routeIter->second.size());
		for (_TyStrategyId id : routeIter->second)
		{
			const auto mIter = m_runtimes.find(id);
			if (m_runtimes.end() != mIter)
			{
				runtimes.emplace_back(mIter->second);
			}
		}
	}
	std::vector<std::pair<int, std::shared_ptr<CStrategyRuntime>>> prioritized;
	prioritized.reserve(runtimes.size());
	for (auto& runtime : runtimes)
	{
		int priority = 3;
		if (StrategyEventType::Quote == ev.m_type)
		{
			for (const auto& instance : runtime->m_securities)
			{
				if (ev.m_security == instance.m_security.String())
				{
					priority = instance.m_strategy->QuotePriority(std::get<_TyQuoteData>(ev.m_data));
					break;
				}
			}
		}
		prioritized.emplace_back(priority, std::move(runtime));
	}
	std::sort(prioritized.begin(), prioritized.end(), [](const auto& left, const auto& right)
	{
		return left.first == right.first ? left.second->m_config.m_id < right.second->m_config.m_id : left.first < right.first;
	});
	for (const auto& item : prioritized)
	{
		const std::shared_ptr<CStrategyRuntime>& runtime = item.second;
		CStrategyEvent routedEvent = ev;
		routedEvent.m_completion = std::make_shared<std::promise<bool>>();
		std::future<bool> completion = routedEvent.m_completion->get_future();
		if (EnqueueEvent(runtime, std::move(routedEvent)))
		{
			completion.get();
		}
	}
}

void CStrategyService::AddRoutes(const CStrategyConfig& cfg)
{
	// Keep local routing and HQ demand in the same order during start/stop.
	// HQService never calls back into StrategyService while holding its state lock.
	std::unique_lock<std::shared_mutex> lock(m_routes_mutex);
	for (const auto& quote : cfg.m_subscriptions)
	{
		m_market_routes[quote.String()].emplace(cfg.m_id);
	}
	if (nullptr != m_hq_service)
	{
		m_hq_service->Subscribe(HQOrigin::Strategy, cfg.m_id, cfg.m_subscriptions);
	}
}

void CStrategyService::RemoveRoutes(const CStrategyConfig& cfg)
{
	std::unique_lock<std::shared_mutex> lock(m_routes_mutex);
	for (const auto& quote : cfg.m_subscriptions)
	{
		const auto mIter = m_market_routes.find(quote.String());
		if (m_market_routes.end() != mIter)
		{
			mIter->second.erase(cfg.m_id);
			if (mIter->second.empty())
			{
				m_market_routes.erase(mIter);
			}
		}
	}
	if (nullptr != m_hq_service)
	{
		m_hq_service->Release(HQOrigin::Strategy, cfg.m_id);
	}
}

COrderSubmitResult CStrategyService::SubmitTradingOrder(const COrderIntent& intent, const std::shared_ptr<CStrategyRuntime>& runtime)
{
	if (!m_trade_service->IsReady(intent.m_account))
	{
		return { false, true, 0, 0, "Account not synchronized" };
	}
	if (OrderType::Limit != intent.m_order_type)
	{
		return { false, false, 0, 0, "UNSUPPORTED: only limit orders" };
	}

	trade::CReqParam request;
	trade::CSubmitParam& params = request.m_param.emplace<trade::CSubmitParam>();
	request.m_account = intent.m_account;
	params.m_order.m_client_id = intent.m_client_order_id;
	request.m_request_id = params.m_order.m_client_id;
	params.m_order.m_id = intent.m_id;
	params.m_order.m_signal_id = intent.m_signal_id;
	params.m_order.m_security = intent.m_security.String();
	params.m_order.m_side = OrderSide::Buy == intent.m_side ? trade::Side::Buy : trade::Side::Sell;
	params.m_order.m_price = intent.m_price;
	params.m_order.m_quantity = intent.m_quantity;
	params.m_order.m_price_scale = intent.m_price_scale;
	std::uint64_t nClientId = params.m_order.m_client_id;
	if (nullptr == runtime || runtime->m_removing.load())
	{
		return { false, false, nClientId, 0, "Strategy is unavailable" };
	}
	CRequest wire = trade::protocol::WriteRequest(request);
	wire.SetExtraData("request_source", "Strategy");
	wire.SetExtraData("strategy_instance_id", std::to_string(runtime->m_instance_id));
	trade::CResult result = m_trade_service->SendStrategyRequest(std::move(wire));
	bool queued = result.Ok();
	return { queued, !queued, nClientId, 0, queued ? "Request queued; await order event" : result.m_message };
}

COrderSubmitResult CStrategyService::SubmitOrder(_TyStrategyId id, const COrderIntent& intent)
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (nullptr == m_trade_service) || (StrategyState::Running != runtime->m_state.load()) || !intent.IsValid() || !m_market_available.load())
	{
		return { false, true, 0, 0, "Strategy or market is unavailable" };
	}
	std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	if ((0 >= intent.m_quote_time_ms) || (5000 < now - intent.m_quote_time_ms) || (1000 < intent.m_quote_time_ms - now))
	{
		return { false, true, 0, 0, "Quote is stale" };
	}
	std::optional<trade::CExecutionRule> rules = m_trade_service->GetExecutionRules(runtime->m_config.m_account, intent.m_security);
	if (!rules.has_value() || !rules->m_reliable_query || !rules->m_idempotent_client_id)
	{
		return { false, true, 0, 0, "Reliable execution rules and fee bounds are unavailable" };
	}
	const trade::CRule& rule = rules->m_rule;
	if (rule.m_corporate_action && (intent.m_basis_version != rule.m_version_text))
	{
		return { false, false, 0, 0, "Price basis needs explicit confirmation" };
	}
	const trade::CCalendar& calendar = rules->m_calendar;
	if ((rule.m_security != intent.m_security.String()) || (rule.m_price_scale != intent.m_price_scale) || (0 >= rule.m_tick) || (0 != intent.m_price % rule.m_tick) || (intent.m_price < rule.m_lower) || (intent.m_price > rule.m_upper) || (now < calendar.m_open_time_ms) || (now >= calendar.m_close_time_ms) || ((now >= calendar.m_break_time_ms) && (now < calendar.m_resume_time_ms)))
	{
		return { false, true, 0, 0, "Price, tick or trading session is invalid" };
	}
	CStrategyAccountView view = m_trade_service->GetStrategyAccountView(runtime->m_config.m_account, intent.m_security);
	bool buy = OrderSide::Buy == intent.m_side;
	std::int64_t minimum = buy ? rule.m_buy_minimum : rule.m_sell_minimum;
	std::int64_t step = buy ? rule.m_buy_step : rule.m_sell_step;
	bool entirePosition = !buy && (intent.m_quantity == view.m_position.m_total_quantity) && (intent.m_quantity <= view.m_position.m_available_quantity);
	if (!entirePosition && ((0 >= minimum) || (0 >= step) || (intent.m_quantity < minimum) || (0 != (intent.m_quantity - minimum) % step)))
	{
		return { false, false, 0, 0, "Quantity violates instrument rules" };
	}
	std::int64_t debit = 0;
	if (buy && (!trade::MaximumDebit(rule, intent.m_price, intent.m_quantity, debit) || (debit != intent.m_maximum_debit)))
	{
		return { false, false, 0, 0, "Fee bound does not match current rules" };
	}
	COrderIntent routed = intent;
	routed.m_id = id;
	routed.m_account = runtime->m_config.m_account;
	COrderSubmitResult prepared = m_execution.Prepare(routed, view, debit);
	if (!prepared.m_accepted)
	{
		return prepared;
	}
	routed.m_client_order_id = prepared.m_client_order_id;
	COrderSubmitResult result = SubmitTradingOrder(routed, runtime);
	// SendStrategyRequest returns failure only before admitting the transport request.
	if (!m_execution.MarkSubmission(routed.m_client_order_id, result.m_accepted, !result.m_accepted))
	{
		result.m_accepted = false;
		result.m_reason = "Cannot persist submission state; reconcile before retry: " + m_execution.GetLastError();
	}
	result.m_client_order_id = routed.m_client_order_id;
	result.m_retryable = false;
	return result;
}

bool CStrategyService::CancelOrder(_TyStrategyId id, _TyOrderId orderId)
{
	if ((nullptr == m_trade_service) || (nullptr == m_account_service) || (0 == orderId))
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}

	{
		std::shared_lock<std::shared_mutex> lock(m_orders_mutex);
		const auto mIter = m_order_routes.find({ runtime->m_config.m_account, orderId });
		if ((m_order_routes.end() == mIter) || (id != mIter->second))
		{
			return false;
		}
	}
	std::vector<trade::COrder> orders = m_trade_service->GetOrders(runtime->m_config.m_account);
	if (!std::any_of(orders.begin(), orders.end(), [id, orderId](const trade::COrder& item)
	{
		return orderId == item.m_order_id && id == item.m_id && trade::IsActive(item.m_status);
	}))
	{
		return false;
	}
	trade::CReqParam request;
	request.m_account = runtime->m_config.m_account;
	request.m_param = trade::CCancelParam{ orderId };
	CRequest wire = trade::protocol::WriteRequest(request);
	wire.SetExtraData("request_source", "Strategy");
	wire.SetExtraData("strategy_id", std::to_string(id));
	wire.SetExtraData("strategy_instance_id", std::to_string(runtime->m_instance_id));
	return m_trade_service->SendStrategyRequest(std::move(wire)).Ok();
}

CStrategySnapshot CStrategyService::MakeSnapshot(const std::shared_ptr<CStrategyRuntime>& runtime) const
{
	CStrategySnapshot snapshot;
	snapshot.m_id = runtime->m_config.m_id;
	snapshot.m_type = runtime->m_config.m_type;
	snapshot.m_name = runtime->m_config.m_name;
	snapshot.m_state = runtime->m_state.load();
	snapshot.m_market_available = m_market_available.load();
	{
		std::lock_guard<std::mutex> lock(runtime->m_events_mutex);
		snapshot.m_queue_length = runtime->m_events.size();
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_orders_mutex);
		snapshot.m_active_order_count = runtime->m_active_order_ids.size();
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_metrics_mutex);
		snapshot.m_last_event_sequence = runtime->m_last_event_sequence;
		snapshot.m_dropped_market_events = runtime->m_dropped_market_events;
		snapshot.m_last_callback_duration_us = runtime->m_last_callback_duration_us;
		snapshot.m_max_callback_duration_us = runtime->m_max_callback_duration_us;
		snapshot.m_last_error = runtime->m_last_error;
		snapshot.m_business_state = runtime->m_business_state;
		snapshot.m_details_json = runtime->m_details_json;
	}
	return snapshot;
}

void CStrategyService::NotifySnapshot(const std::shared_ptr<CStrategyRuntime>& runtime)
{
	CStrategySnapshot snapshot = MakeSnapshot(runtime);
	std::vector<_TyStateHandler> handlers;
	{
		std::lock_guard<std::mutex> lock(m_handlers_mutex);
		handlers = m_state_handlers;
	}
	for (const auto& handler : handlers)
	{
		if (nullptr != handler)
		{
			try
			{
				handler(snapshot);
			}
			catch (...)
			{
			}
		}
	}
}

void CStrategyService::WaitUntilIdle(const std::shared_ptr<CStrategyRuntime>& runtime)
{
	std::unique_lock<std::mutex> lock(runtime->m_events_mutex);
	runtime->m_idle_condition.wait(lock, [&runtime]()
	{
		return runtime->m_events.empty() && !runtime->m_scheduled && !runtime->m_executing_callback;
	});
}

std::string CStrategyService::GetMarketKey(const CRequest& req)
{
	const _TyReqData& data = req.GetData();
	if (data.has_quote())
	{
		return MakeQuoteInfo(data.quote().security(), Channel::quote).String();
	}
	if (data.has_depth())
	{
		return MakeQuoteInfo(data.depth().security(), Channel::depth).String();
	}
	if (data.has_trade())
	{
		return MakeQuoteInfo(data.trade().security(), Channel::trade).String();
	}
	if (data.has_bar())
	{
		Channel channel = static_cast<Channel>(static_cast<int>(data.bar().channel()));
		return MakeQuoteInfo(data.bar().security(), channel).String();
	}
	return {};
}

bool CStrategyService::IsFinishedOrderStatus(OrderStatus status)
{
	return (OrderStatus::Rejected == status) || (OrderStatus::Filled == status) || (OrderStatus::Cancelled == status) || (OrderStatus::Expired == status);
}

bool CStrategyService::ActionStrategy(const CRequest& req)
{
	std::uint64_t id = 0;
	if (!utility::to_number(req.GetExtraData("strategy_id"), id) || (0 == id) || (id == currentStrategyId))
	{
		net::SendError(req.GetConnectionId(), req, 1101, "Invalid strategy ID");
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		net::SendError(req.GetConnectionId(), req, 1110, "Strategy not found");
		return false;
	}
	if ("reconcile" == req.GetExtraData("action"))
	{
		m_trade_service->ReconcileAccount(runtime->m_config.m_account);
		return SendStrategyResponse(req, "Reconciliation requested");
	}
	CStrategyEvent event;
	event.m_type = StrategyEventType::Action;
	event.m_action = req.GetExtraData("action");
	event.m_completion = std::make_shared<std::promise<bool>>();
	std::future<bool> result = event.m_completion->get_future();
	if (!EnqueueEvent(runtime, std::move(event)) || !result.get())
	{
		net::SendError(req.GetConnectionId(), req, 1112, "Action blocked by unresolved task or order");
		return false;
	}
	return SendStrategyResponse(req, "Action completed");
}

bool CStrategyService::RequestHistory(_TyStrategyId id, const CSecurity& security, std::int64_t begin)
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (nullptr == m_hq_service))
	{
		return false;
	}
	strategy::CSettings settings;
	std::string error;
	if (!strategy::CSettings::Read(runtime->m_config.m_type, runtime->m_config.m_params, settings, error) || (strategy::Kind::DualMaCross != settings.m_kind))
	{
		return false;
	}
	std::int64_t end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	if (0 == begin)
	{
		begin = std::max<std::int64_t>(0, end - static_cast<std::int64_t>(settings.m_long_window) * ("1m" == settings.m_bar_period ? 60000 : 86400000) * 4);
	}
	CQuoteInfo quote(security.m_code, security.m_market, "1m" == settings.m_bar_period ? Channel::bar_1m : Channel::bar_1d);
	std::weak_ptr<CStrategyRuntime> weak = runtime;
	return m_hq_service->QueryBars(quote, begin, end, [this, weak, security](const CRequest& response)
	{
		std::shared_ptr<CStrategyRuntime> target = weak.lock();
		if ((nullptr == target) || target->m_removing.load())
		{
			return;
		}
		if (!response.GetData().has_query_response())
		{
			std::lock_guard<std::mutex> lock(target->m_metrics_mutex);
			target->m_last_error = security.String() + ": Historical K-line query failed";
			return;
		}
		std::map<std::int64_t, _TyBarData> ordered;
		for (const auto& bar : response.GetData().query_response().bars())
		{
			const auto mIter = ordered.find(bar.begin_time_ms());
			if ((ordered.end() == mIter) || (mIter->second.data_version() < bar.data_version()))
			{
				ordered.insert_or_assign(bar.begin_time_ms(), bar);
			}
		}
		std::vector<_TyBarData> bars;
		bars.reserve(ordered.size());
		for (auto& [time, bar] : ordered)
		{
			bars.emplace_back(std::move(bar));
		}
		CStrategyEvent event;
		event.m_type = StrategyEventType::History;
		event.m_security = security.String();
		event.m_data = std::move(bars);
		EnqueueEvent(target, std::move(event));
	});
}
