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
		cfg.m_strType = info.strategy_type();
		cfg.m_revision = info.config_revision();
		cfg.m_strName = info.strategy_name();
		cfg.m_eventQueueLimit = (0 == info.event_queue_limit()) ? 4096 : info.event_queue_limit();
		cfg.m_bAutoStart = info.auto_start();
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
		return cfg.IsValid() && (1000000 >= cfg.m_eventQueueLimit) && (!info.auto_start() || info.enabled()) && CConfiguredStrategy::ValidateParameters(cfg, error);
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
			pItem->set_security(quote.m_security.m_strCode);
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
	return (m_id == arg.m_id) && (m_signalId == arg.m_signalId);
}

std::size_t CSignalKeyHash::operator()(const CSignalKey& key) const
{
	std::size_t strategyHash = std::hash<_TyStrategyId>{}(key.m_id);
	std::size_t signalHash = std::hash<_TySignalId>{}(key.m_signalId);
	return strategyHash ^ (signalHash + 0x9e3779b9U + (strategyHash << 6) + (strategyHash >> 2));
}

CStrategyService::CStrategyService(CHQService* pHQService, CTradeService* pTradeService, CAccountService* pAccountService, CIndicator& indicator) : m_indicator(indicator), m_pHQService(pHQService), m_pTradeService(pTradeService), m_pAccountService(pAccountService)
{
}

CStrategyService::~CStrategyService()
{
	StopAll();
	if (nullptr != m_pTradeService)
	{
		m_pTradeService->SetStrategyReplyHandler({});
		m_pTradeService->SetEventHandler({});
		m_pTradeService->SetAccountReadyHandler({});
	}
}

bool CStrategyService::Initialize()
{
	m_strLastError.clear();
	if (!m_execution.Open())
	{
		m_strLastError = m_execution.GetLastError();
		return false;
	}
	if (nullptr == m_pHQService || nullptr == m_pTradeService || nullptr == m_pAccountService)
	{
		m_strLastError = "Strategy market or trade service is unavailable";
		return false;
	}
	m_pTradeService->SetStrategyReplyHandler(std::bind_front(static_cast<void (CStrategyService::*)(const CRequest&, const trade::CCounterReply&)>(&CStrategyService::OnReply), this));
	m_pTradeService->SetEventHandler(std::bind_front(static_cast<void (CStrategyService::*)(const trade::CCounterEvent&)>(&CStrategyService::OnTradeEvent), this));
	m_pTradeService->SetAccountReadyHandler(std::bind_front(&CStrategyService::OnAccountReady, this));
	return LoadStrategy();
}

bool CStrategyService::BeginAccountUse(const _TyAccount& account, _TyStrategyId strategyId)
{
	std::lock_guard<std::mutex> lock(m_mtx_accountUsers);
	if (container::with(m_runningStrategies[account], strategyId) || !m_pTradeService->IsReady(account) || !m_pAccountService->AcquireAccount(account))
	{
		return false;
	}
	m_runningStrategies[account].emplace(strategyId);
	return true;
}

void CStrategyService::EndAccountUse(const _TyAccount& account, _TyStrategyId strategyId)
{
	std::lock_guard<std::mutex> lock(m_mtx_accountUsers);
	const auto mIter = m_runningStrategies.find(account);
	if (m_runningStrategies.end() != mIter && 0 != mIter->second.erase(strategyId))
	{
		m_pAccountService->ReleaseAccount(account);
		if (mIter->second.empty())
		{
			m_runningStrategies.erase(mIter);
		}
	}
}

void CStrategyService::OnReply(const CRequest& origin, const trade::CCounterReply& reply)
{
	std::uint64_t strategyId = 0;
	std::uint64_t instanceId = 0;
	if (!tradingroute::ReadNumber(origin, "strategy_id", strategyId) || !tradingroute::ReadNumber(origin, "strategy_instance_id", instanceId))
	{
		std::cerr << "Invalid strategy reply routing metadata\n";
		return;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(strategyId);
	if (nullptr == runtime || runtime->m_bRemoving.load() || instanceId != runtime->m_instance_id || reply.m_account != runtime->m_config.m_account)
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
		std::lock_guard<std::mutex> lock(runtime->m_mtx_metrics);
		runtime->m_strLastError = reply.m_result.m_strMessage;
	}
	NotifySnapshot(runtime);
	if (trade::Operation::Submit != reply.m_operation || (trade::Error::Rejected != reply.m_result.m_error && trade::Error::Unsupported != reply.m_result.m_error && trade::Error::Conflict != reply.m_result.m_error && trade::Error::NotFound != reply.m_result.m_error))
	{
		return;
	}
	std::uint64_t rejectedClient = 0;
	if (tradingroute::ReadNumber(origin, "client_order_id", rejectedClient))
	{
		m_execution.RejectBeforeAcceptance(rejectedClient, reply.m_result.m_strMessage);
	}
	COrderEvent event;
	event.m_account = reply.m_account;
	event.m_id = strategyId;
	event.m_status = OrderStatus::Rejected;
	event.m_strReason = reply.m_result.m_strMessage;
	std::uint64_t quantity = 0;
	if (!tradingroute::ReadNumber(origin, "signal_id", event.m_signalId) || !tradingroute::ReadNumber(origin, "client_order_id", event.m_clientOrderId) || !tradingroute::ReadNumber(origin, "quantity", quantity) || static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) < quantity)
	{
		std::cerr << "Invalid rejected strategy order metadata\n";
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
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
		const auto mIter = m_runtimes.find(strategyId);
		if (m_runtimes.end() == mIter || mIter->second != runtime || runtime->m_bRemoving.load())
		{
			return;
		}
		std::lock_guard<std::mutex> signalLock(m_mtx_signals);
		if (0 != event.m_signalId)
		{
			m_signals[{ strategyId, event.m_signalId }] = SignalState::Rejected;
		}
	}
	CStrategyEvent strategyEvent;
	strategyEvent.m_type = StrategyEventType::Order;
	strategyEvent.m_data = std::move(event);
	EnqueueEvent(runtime, std::move(strategyEvent));
}

void CStrategyService::OnTradeEvent(const trade::CCounterEvent& event)
{
	m_execution.Reconcile(event.m_account, m_pTradeService->GetStrategyAccountView(event.m_account, {}));
	std::vector<std::shared_ptr<CStrategyRuntime>> affected;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
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
		timer.m_zeroPosition = (trade::EventTy::Position == event.m_kind) && (0 == event.m_position.m_nTotal) && !event.m_position.m_strSecurity.empty();
		if (trade::EventTy::Position == event.m_kind)
		{
			tick.m_security = event.m_position.m_strSecurity;
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
	m_execution.Reconcile(account, m_pTradeService->GetStrategyAccountView(account, {}));
	if (m_bStopping.load())
	{
		return;
	}
	std::vector<_TyStrategyId> deferred;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
		for (const auto& item : m_runtimes)
		{
			if (account == item.second->m_config.m_account && item.second->m_config.m_bAutoStart && StrategyState::Initialized == item.second->m_state.load())
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
	std::lock_guard<std::mutex> operationLock(m_mtx_operations);
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
	if (!m_pAccountService->HasAccount(FmtAccount(info)))
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
	if (!m_pAccountService->HasAccount(FmtAccount(info)))
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
			std::lock_guard<std::mutex> lock(oldRuntime->m_mtx_orders);
			ownActiveOrders = !oldRuntime->m_activeOrderIds.empty();
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
	staged.m_bAutoStart = false;
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
		std::unique_lock<std::shared_mutex> lock(m_mtx_strategies);
		const auto mIter = m_runtimes.find(cfg.m_id);
		if (m_runtimes.end() != mIter)
		{
			mIter->second->m_config.m_revision = info.config_revision();
			mIter->second->m_config.m_bAutoStart = cfg.m_bAutoStart;
		}
	}
	if (info.enabled() && cfg.m_bAutoStart && !StartStrategy(cfg.m_id))
	{
		m_strLastError = "Configuration saved; automatic start requires attention";
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
			if (CreateStrategy(oldConfig) && !oldConfig.m_bAutoStart && ((StrategyState::Running == oldState) || (StrategyState::Starting == oldState)))
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
		pRecord->set_market_available(snapshot.m_bMarketAvailable);
		pRecord->set_queue_length(snapshot.m_queueLength);
		pRecord->set_active_orders(snapshot.m_activeOrderCount);
		pRecord->set_last_error(snapshot.m_strLastError);
		pRecord->set_business_state(snapshot.m_businessState);
		pRecord->set_details_json(snapshot.m_detailsJson);
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
	if ((_TyRequest::STRATEGY_START == type) && (nullptr != runtime) && !m_pTradeService->IsReady(runtime->m_config.m_account))
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
	return m_strLastError;
}

bool CStrategyService::LoadStrategy()
{
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		m_strLastError = "Strategy database is unavailable";
		return false;
	}
	const db::CTable& test = pDB->ExecQuery("SHOW TABLES LIKE 'table_strategy'");
	if (test.m_rows.empty())
	{
		m_strLastError = "MySQL table_strategy does not exist";
		return false;
	}
	const db::CTable& table = pDB->ExecQuery("SELECT strategy_id,strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,account_identity,config_revision FROM table_strategy WHERE enabled=1 ORDER BY strategy_id");

	std::vector<std::string> row;
	for (const db::_TyRow& item : table.m_rows)
	{
		if (9 != db::to_string_row(item, row))
		{
			m_strLastError = "table_strategy contains an invalid row";
			return false;
		}
		CStrategyConfig cfg;
		std::uint64_t id = 0;
		std::uint64_t eventQueueLimit = 0;
		std::uint64_t autoStart = 0;
		if (!utility::to_number(row[0], id) || !utility::to_number(row[5], eventQueueLimit) || !utility::to_number(row[6], autoStart))
		{
			m_strLastError = "table_strategy contains invalid numeric fields";
			return false;
		}
		cfg.m_id = id;
		if (!utility::to_number(row[8], cfg.m_revision) || (0 == cfg.m_revision))
		{
			return false;
		}
		cfg.m_strType = row[1];
		cfg.m_strName = row[2];
		cfg.m_eventQueueLimit = static_cast<std::size_t>(eventQueueLimit);
		cfg.m_bAutoStart = 0 != autoStart;
		if (!DecodeAccount(row[7], cfg.m_account))
		{
			m_strLastError = "Invalid strategy trading account";
			return false;
		}
		if (!ParseParameters(row[3], cfg) || !ParseSubscriptions(row[4], cfg) || !CreateStrategy(cfg))
		{
			m_strLastError = "Failed to load strategy " + row[0] + " from table_strategy";
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
	if (!cfg.IsValid() || m_bStopping.load())
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
		for (auto& [k, v] : subscriptions)
		{
			CStrategyStockInstance& instance = runtime->m_securities.emplace_back();
			instance.m_security = v.front().m_security;
			instance.m_pStrategy.reset(CreateStrategy(cfg.m_strType));
			instance.m_pContext = std::make_unique<CStrategyContext>(cfg.m_id, cfg.m_account, this, m_pTradeService, instance.m_security, 1 < subscriptions.size());
			CStrategyConfig config = cfg;
			config.m_subscriptions = std::move(v);
			if ((nullptr == instance.m_pStrategy) || !instance.m_pStrategy->Initialize(*instance.m_pContext, config))
			{
				return false;
			}
		}
	}
	catch (const std::exception& error)
	{
		m_strLastError = error.what();
		return false;
	}
	runtime->m_state.store(StrategyState::Initialized);

	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_strategies);
		if (!m_runtimes.emplace(cfg.m_id, runtime).second)
		{
			return false;
		}
	}

	for (const auto& order : m_pTradeService->GetOrders(cfg.m_account))
	{
		if (cfg.m_id != order.m_id)
		{
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(m_mtx_signals);
			m_signals[{ cfg.m_id, order.m_signalId }] = SignalState::Accepted;
		}
		if (trade::IsActive(order.m_status))
		{
			{
				std::lock_guard<std::mutex> lock(runtime->m_mtx_orders);
				runtime->m_activeOrderIds.emplace(order.m_orderId);
			}
			{
				std::unique_lock<std::shared_mutex> lock(m_mtx_orders);
				m_orderRoutes[{ order.m_account, order.m_orderId }] = cfg.m_id;
				m_clientOrderRoutes[{ order.m_account, order.m_clientId }] = cfg.m_id;
			}
		}
	}
	NotifySnapshot(runtime);
	if (cfg.m_bAutoStart && m_pTradeService->IsReady(cfg.m_account) && !StartStrategy(cfg.m_id))
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
		std::lock_guard<std::mutex> lock(runtime->m_mtx_orders);
		if (!runtime->m_activeOrderIds.empty())
		{
			return false;
		}
	}
	runtime->m_bRemoving.store(true);
	runtime->m_bAcceptMarketEvents.store(false);
	RemoveRoutes(runtime->m_config);
	WaitUntilIdle(runtime);
	for (auto& stockInstance : runtime->m_securities)
	{
		stockInstance.m_pContext->Disable();
	}
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_strategies);
		m_runtimes.erase(id);
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_signals);
		std::erase_if(m_signals, [id](const auto& item)
		{
			return id == item.first.m_id;
		});
	}
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_orders);
		std::erase_if(m_orderRoutes, [id](const auto& item)
		{
			return id == item.second;
		});
		std::erase_if(m_clientOrderRoutes, [id](const auto& item)
		{
			return id == item.second;
		});
	}
	return true;
}

bool CStrategyService::StartStrategy(_TyStrategyId id)
{
	if (m_bStopping.load() || nullptr == m_pTradeService || nullptr == m_pAccountService)
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
	runtime->m_bAcceptMarketEvents.store(false);
	if (!ExecuteControl(id, StrategyEventType::Pause))
	{
		runtime->m_state.store(StrategyState::Running);
		runtime->m_bAcceptMarketEvents.store(true);
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
	runtime->m_bAcceptMarketEvents.store(false);
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
	if (m_bStopping.exchange(true))
	{
		return;
	}
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
		runtimes.reserve(m_runtimes.size());
		for (const auto& item : m_runtimes)
		{
			runtimes.emplace_back(item.second);
		}
	}
	for (const auto& runtime : runtimes)
	{
		runtime->m_bAcceptMarketEvents.store(false);
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
			stockInstance.m_pContext->Disable();
		}
		EndAccountUse(runtime->m_config.m_account, runtime->m_config.m_id);
	}
}

bool CStrategyService::OnReply(const CRequest& req)
{
	if (m_bStopping.load() || !m_bMarketAvailable.load() || (!req.IsMarket()))
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
	m_bMarketAvailable.store(bAvailable);
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
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
			std::lock_guard<std::mutex> lock(runtime->m_mtx_metrics);
			runtime->m_strLastError = strReason;
		}
		NotifySnapshot(runtime);
	}
}

void CStrategyService::OnOrderEvent(const COrderEvent& ev)
{
	_TyStrategyId id = ev.m_id;
	if (0 == id)
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_orders);
		const auto mIter = m_orderRoutes.find({ ev.m_account, ev.m_orderId });
		if (m_orderRoutes.end() != mIter)
		{
			id = mIter->second;
		}
		else
		{
			const auto clientIter = m_clientOrderRoutes.find({ ev.m_account, ev.m_clientOrderId });
			if (m_clientOrderRoutes.end() != clientIter)
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
	if (0 != ev.m_signalId)
	{
		std::lock_guard<std::mutex> lock(m_mtx_signals);
		m_signals[{ id, ev.m_signalId }] = OrderStatus::Rejected == ev.m_status ? SignalState::Rejected : SignalState::Accepted;
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_mtx_orders);
		if (IsFinishedOrderStatus(ev.m_status))
		{
			runtime->m_activeOrderIds.erase(ev.m_orderId);
		}
		else if (0 != ev.m_orderId)
		{
			runtime->m_activeOrderIds.emplace(ev.m_orderId);
		}
	}
	if (!IsFinishedOrderStatus(ev.m_status) && 0 != ev.m_orderId)
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_orders);
		m_orderRoutes[{ ev.m_account, ev.m_orderId }] = id;
		m_clientOrderRoutes[{ ev.m_account, ev.m_clientOrderId }] = id;
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
		std::shared_lock<std::shared_mutex> lock(m_mtx_orders);
		const auto mIter = m_orderRoutes.find({ ev.m_account, ev.m_orderId });
		if (m_orderRoutes.end() != mIter)
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
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
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
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_stateHandlers.emplace_back(std::move(handler));
}

bool CStrategyService::IsMarketAvailable() const
{
	return m_bMarketAvailable.load();
}

std::shared_ptr<CStrategyRuntime> CStrategyService::FindRuntime(_TyStrategyId id) const
{
	std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
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
	if ((nullptr == runtime) || runtime->m_bRemoving.load() || (ev.IsMarketEvent() && !runtime->m_bAcceptMarketEvents.load()))
	{
		return false;
	}
	bool bSchedule = false;
	bool bFault = false;
	{
		std::lock_guard<std::mutex> lock(runtime->m_mtx_events);
		if (runtime->m_config.m_eventQueueLimit <= runtime->m_events.size())
		{
			const auto vIter = std::find_if(runtime->m_events.begin(), runtime->m_events.end(), [](const CStrategyEvent& queuedEvent)
			{
				return queuedEvent.IsMarketEvent();
			});
			if (runtime->m_events.end() != vIter)
			{
				runtime->m_events.erase(vIter);
				std::lock_guard<std::mutex> metricsLock(runtime->m_mtx_metrics);
				++runtime->m_droppedMarketEvents;
			}
			else if ((runtime->m_config.m_eventQueueLimit * 2) <= runtime->m_events.size())
			{
				bFault = true;
			}
		}
		runtime->m_events.emplace_back(std::move(ev));
		if (!runtime->m_bScheduled)
		{
			runtime->m_bScheduled = true;
			bSchedule = true;
		}
	}
	if (bFault)
	{
		HandleStrategyException(*runtime, "Strategy critical event queue exceeded hard limit");
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
			std::lock_guard<std::mutex> lock(runtime->m_mtx_events);
			if (runtime->m_events.empty())
			{
				runtime->m_bScheduled = false;
				runtime->m_bExecutingCallback = false;
				runtime->m_cvIdle.notify_all();
				return;
			}
			event = std::move(runtime->m_events.front());
			runtime->m_events.pop_front();
			runtime->m_bExecutingCallback = true;
		}
		bool bResult = DispatchEvent(*runtime, event);
		if (nullptr != event.m_completion)
		{
			event.m_completion->set_value(bResult);
		}
		{
			std::lock_guard<std::mutex> lock(runtime->m_mtx_events);
			runtime->m_bExecutingCallback = false;
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
				bResult = instance.m_pStrategy->OnAction(event.m_action) && bResult;
				break;
			case StrategyEventType::History:
				instance.m_pStrategy->OnHistory(std::get<std::vector<_TyBarData>>(event.m_data));
				break;
			case StrategyEventType::MarketState:
				instance.m_pStrategy->OnMarketState(std::get<bool>(event.m_data));
				break;
			case StrategyEventType::Start:
				instance.m_pStrategy->OnStart();
				break;
			case StrategyEventType::Pause:
				instance.m_pStrategy->OnPause();
				break;
			case StrategyEventType::Stop:
				instance.m_pStrategy->OnStop();
				break;
			case StrategyEventType::Quote:
				instance.m_pStrategy->OnQuote(std::get<_TyQuoteData>(event.m_data));
				break;
			case StrategyEventType::Depth:
				instance.m_pStrategy->OnDepth(std::get<_TyDepthData>(event.m_data));
				break;
			case StrategyEventType::MarketTrade:
				instance.m_pStrategy->OnMarketTrade(std::get<_TyMarketTradeData>(event.m_data));
				break;
			case StrategyEventType::Bar:
				instance.m_pStrategy->OnBar(std::get<_TyBarData>(event.m_data));
				break;
			case StrategyEventType::Order:
				if (instance.m_security == std::get<COrderEvent>(event.m_data).m_security)
				{
					instance.m_pStrategy->OnOrder(std::get<COrderEvent>(event.m_data));
				}
				break;
			case StrategyEventType::Trade:
				if (instance.m_security == std::get<CTradeEvent>(event.m_data).m_security)
				{
					instance.m_pStrategy->OnTrade(std::get<CTradeEvent>(event.m_data));
				}
				break;
			case StrategyEventType::Timer:
				instance.m_pStrategy->OnTimer(std::get<CTimerEvent>(event.m_data));
				break;
			}
		}
		if (StrategyEventType::Start == event.m_type)
		{
			runtime.m_state.store(StrategyState::Running);
			runtime.m_bAcceptMarketEvents.store(true);
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
		std::lock_guard<std::mutex> lock(runtime.m_mtx_metrics);
		runtime.m_lastEventSequence = event.m_sequence;
		runtime.m_lastCallbackDurationUs = duration;
		runtime.m_maxCallbackDurationUs = (std::max)(runtime.m_maxCallbackDurationUs, duration);
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
	runtime.m_bAcceptMarketEvents.store(false);
	runtime.m_state.store(StrategyState::Faulted);
	EndAccountUse(runtime.m_config.m_account, runtime.m_config.m_id);
	std::lock_guard<std::mutex> lock(runtime.m_mtx_metrics);
	runtime.m_strLastError = strError;
}

void CStrategyService::RouteMarketEvent(const std::string& strKey, CStrategyEvent&& ev)
{
	std::vector<std::shared_ptr<CStrategyRuntime>> runtimes;
	{
		std::shared_lock<std::shared_mutex> routesLock(m_mtx_routes);
		const auto routeIter = m_marketRoutes.find(strKey);
		if (m_marketRoutes.end() == routeIter)
		{
			return;
		}
		std::shared_lock<std::shared_mutex> strategiesLock(m_mtx_strategies);
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
	std::sort(runtimes.begin(), runtimes.end(), [&ev](const auto& left, const auto& right)
	{
		auto priority = [&ev](const std::shared_ptr<CStrategyRuntime>& runtime)
		{
			for (const auto& instance : runtime->m_securities)
			{
				if (ev.m_security == instance.m_security.String())
				{
					return instance.m_pStrategy->QuotePriority(std::get<_TyQuoteData>(ev.m_data));
				}
			}
			return 3;
		};
		int a = StrategyEventType::Quote == ev.m_type ? priority(left) : 3;
		int b = StrategyEventType::Quote == ev.m_type ? priority(right) : 3;
		return a == b ? left->m_config.m_id < right->m_config.m_id : a < b;
	});
	for (const auto& runtime : runtimes)
	{
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
	std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
	for (const auto& quote : cfg.m_subscriptions)
	{
		m_marketRoutes[quote.String()].emplace(cfg.m_id);
	}
	if (nullptr != m_pHQService)
	{
		m_pHQService->Subscribe(HQOrigin::Strategy, cfg.m_id, cfg.m_subscriptions);
	}
}

void CStrategyService::RemoveRoutes(const CStrategyConfig& cfg)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
	for (const auto& quote : cfg.m_subscriptions)
	{
		const auto mIter = m_marketRoutes.find(quote.String());
		if (m_marketRoutes.end() != mIter)
		{
			mIter->second.erase(cfg.m_id);
			if (mIter->second.empty())
			{
				m_marketRoutes.erase(mIter);
			}
		}
	}
	if (nullptr != m_pHQService)
	{
		m_pHQService->Release(HQOrigin::Strategy, cfg.m_id);
	}
}

COrderSubmitResult CStrategyService::SubmitTradingOrder(const COrderIntent& intent, const std::shared_ptr<CStrategyRuntime>& runtime)
{
	if (!m_pTradeService->IsReady(intent.m_account))
	{
		return { false, true, 0, 0, "Account not synchronized" };
	}
	if (OrderType::Limit != intent.m_orderType)
	{
		return { false, false, 0, 0, "UNSUPPORTED: only limit orders" };
	}

	trade::CReqParam request;
	trade::CSubmitParam& params = request.m_param.emplace<trade::CSubmitParam>();
	request.m_account = intent.m_account;
	params.m_order.m_clientId = intent.m_clientOrderId;
	request.m_requestId = params.m_order.m_clientId;
	params.m_order.m_id = intent.m_id;
	params.m_order.m_signalId = intent.m_signalId;
	params.m_order.m_strSecurity = intent.m_security.String();
	params.m_order.m_side = OrderSide::Buy == intent.m_side ? trade::Side::Buy : trade::Side::Sell;
	params.m_order.m_nPrice = intent.m_price;
	params.m_order.m_nQuantity = intent.m_quantity;
	params.m_order.m_nPriceScale = intent.m_nPriceScale;
	std::uint64_t nClientId = params.m_order.m_clientId;
	if (nullptr == runtime || runtime->m_bRemoving.load())
	{
		return { false, false, nClientId, 0, "Strategy is unavailable" };
	}
	CRequest wire = trade::protocol::WriteRequest(request);
	wire.SetExtraData("request_source", "Strategy");
	wire.SetExtraData("strategy_instance_id", std::to_string(runtime->m_instance_id));
	trade::CResult result = m_pTradeService->SendStrategyRequest(std::move(wire));
	return { result.Ok(), !result.Ok(), nClientId, 0, result.Ok() ? "Request queued; await order event" : result.m_strMessage };
}

COrderSubmitResult CStrategyService::SubmitOrder(_TyStrategyId id, const COrderIntent& intent)
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (nullptr == m_pTradeService) || (StrategyState::Running != runtime->m_state.load()) || !intent.IsValid() || !m_bMarketAvailable.load())
	{
		return { false, true, 0, 0, "Strategy or market is unavailable" };
	}
	std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	if ((0 >= intent.m_quoteTimeMs) || (5000 < now - intent.m_quoteTimeMs) || (1000 < intent.m_quoteTimeMs - now))
	{
		return { false, true, 0, 0, "Quote is stale" };
	}
	std::optional<trade::CExecutionRule> rules = m_pTradeService->GetExecutionRules(runtime->m_config.m_account, intent.m_security);
	if (!rules.has_value() || !rules->m_reliableQuery || !rules->m_idempotentClientId)
	{
		return { false, true, 0, 0, "Reliable execution rules and fee bounds are unavailable" };
	}
	const trade::CRule& rule = rules->m_rule;
	if (rule.m_bCorporateAction && (intent.m_basisVersion != rule.m_strVersion))
	{
		return { false, false, 0, 0, "Price basis needs explicit confirmation" };
	}
	const trade::CCalendar& calendar = rules->m_calendar;
	if ((rule.m_strSecurity != intent.m_security.String()) || (rule.m_nPriceScale != intent.m_nPriceScale) || (0 >= rule.m_nTick) || (0 != intent.m_price % rule.m_nTick) || (intent.m_price < rule.m_nLower) || (intent.m_price > rule.m_nUpper) || (now < calendar.m_nOpen) || (now >= calendar.m_nClose) || ((now >= calendar.m_nBreak) && (now < calendar.m_nResume)))
	{
		return { false, true, 0, 0, "Price, tick or trading session is invalid" };
	}
	CStrategyAccountView view = m_pTradeService->GetStrategyAccountView(runtime->m_config.m_account, intent.m_security);
	bool buy = OrderSide::Buy == intent.m_side;
	std::int64_t minimum = buy ? rule.m_nBuyMinimum : rule.m_nSellMinimum;
	std::int64_t step = buy ? rule.m_nBuyStep : rule.m_nSellStep;
	bool entirePosition = !buy && (intent.m_quantity == view.m_position.m_totalQuantity) && (intent.m_quantity <= view.m_position.m_availableQuantity);
	if (!entirePosition && ((0 >= minimum) || (0 >= step) || (intent.m_quantity < minimum) || (0 != (intent.m_quantity - minimum) % step)))
	{
		return { false, false, 0, 0, "Quantity violates instrument rules" };
	}
	std::int64_t debit = 0;
	if (buy && (!trade::MaximumDebit(rule, intent.m_price, intent.m_quantity, debit) || (debit != intent.m_maximumDebit)))
	{
		return { false, false, 0, 0, "Fee bound does not match current rules" };
	}
	COrderIntent routed = intent;
	routed.m_id = id;
	routed.m_account = runtime->m_config.m_account;
	COrderSubmitResult prepared = m_execution.Prepare(routed, view, debit);
	if (!prepared.m_bAccepted)
	{
		return prepared;
	}
	routed.m_clientOrderId = prepared.m_clientOrderId;
	COrderSubmitResult result = SubmitTradingOrder(routed, runtime);
	// SendStrategyRequest returns failure only before admitting the transport request.
	m_execution.MarkSubmission(routed.m_clientOrderId, result.m_bAccepted, !result.m_bAccepted);
	result.m_clientOrderId = routed.m_clientOrderId;
	result.m_bRetryable = false;
	return result;
}

bool CStrategyService::CancelOrder(_TyStrategyId id, _TyOrderId orderId)
{
	if ((nullptr == m_pTradeService) || (nullptr == m_pAccountService) || (0 == orderId))
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}

	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_orders);
		const auto mIter = m_orderRoutes.find({ runtime->m_config.m_account, orderId });
		if ((m_orderRoutes.end() == mIter) || (id != mIter->second))
		{
			return false;
		}
	}
	std::vector<trade::COrder> orders = m_pTradeService->GetOrders(runtime->m_config.m_account);
	if (!std::any_of(orders.begin(), orders.end(), [id, orderId](const trade::COrder& order)
	{
		return orderId == order.m_orderId && id == order.m_id && trade::IsActive(order.m_status);
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
	return m_pTradeService->SendStrategyRequest(std::move(wire)).Ok();
}

CStrategySnapshot CStrategyService::MakeSnapshot(const std::shared_ptr<CStrategyRuntime>& runtime) const
{
	CStrategySnapshot snapshot;
	snapshot.m_id = runtime->m_config.m_id;
	snapshot.m_strType = runtime->m_config.m_strType;
	snapshot.m_strName = runtime->m_config.m_strName;
	snapshot.m_state = runtime->m_state.load();
	snapshot.m_bMarketAvailable = m_bMarketAvailable.load();
	{
		std::lock_guard<std::mutex> lock(runtime->m_mtx_events);
		snapshot.m_queueLength = runtime->m_events.size();
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_mtx_orders);
		snapshot.m_activeOrderCount = runtime->m_activeOrderIds.size();
	}
	{
		std::lock_guard<std::mutex> lock(runtime->m_mtx_metrics);
		snapshot.m_lastEventSequence = runtime->m_lastEventSequence;
		snapshot.m_droppedMarketEvents = runtime->m_droppedMarketEvents;
		snapshot.m_lastCallbackDurationUs = runtime->m_lastCallbackDurationUs;
		snapshot.m_maxCallbackDurationUs = runtime->m_maxCallbackDurationUs;
		snapshot.m_strLastError = runtime->m_strLastError;
		snapshot.m_businessState = runtime->m_businessState;
		snapshot.m_detailsJson = runtime->m_detailsJson;
	}
	return snapshot;
}

void CStrategyService::NotifySnapshot(const std::shared_ptr<CStrategyRuntime>& runtime)
{
	CStrategySnapshot snapshot = MakeSnapshot(runtime);
	std::vector<_TyStateHandler> handlers;
	{
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		handlers = m_stateHandlers;
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
	std::unique_lock<std::mutex> lock(runtime->m_mtx_events);
	runtime->m_cvIdle.wait(lock, [&runtime]()
	{
		return runtime->m_events.empty() && !runtime->m_bScheduled && !runtime->m_bExecutingCallback;
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
		m_pTradeService->ReconcileAccount(runtime->m_config.m_account);
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
	if ((nullptr == runtime) || (nullptr == m_pHQService))
	{
		return false;
	}
	strategy::CSettings settings;
	std::string error;
	if (!strategy::CSettings::Read(runtime->m_config.m_strType, runtime->m_config.m_params, settings, error) || (strategy::Kind::DualMaCross != settings.m_kind))
	{
		return false;
	}
	std::int64_t end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	if (0 == begin)
	{
		begin = std::max<std::int64_t>(0, end - static_cast<std::int64_t>(settings.m_longWindow) * ("1m" == settings.m_barPeriod ? 60000 : 86400000) * 4);
	}
	CQuoteInfo quote(security.m_strCode, security.m_market, "1m" == settings.m_barPeriod ? Channel::bar_1m : Channel::bar_1d);
	std::weak_ptr<CStrategyRuntime> weak = runtime;
	return m_pHQService->QueryBars(quote, begin, end, [this, weak, security](const CRequest& response)
	{
		std::shared_ptr<CStrategyRuntime> target = weak.lock();
		if ((nullptr == target) || target->m_bRemoving.load())
		{
			return;
		}
		if (!response.GetData().has_query_response())
		{
			std::lock_guard<std::mutex> lock(target->m_mtx_metrics);
			target->m_strLastError = security.String() + ": Historical K-line query failed";
			return;
		}
		std::map<std::int64_t, _TyBarData> ordered;
		for (const auto& bar : response.GetData().query_response().bars())
		{
			ordered.insert_or_assign(bar.begin_time_ms(), bar);
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
