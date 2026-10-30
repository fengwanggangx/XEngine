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
#include "CStrategyContext.h"
#include "CTradeService.h"
#include "strategies/CMovingAverageStrategy.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
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

	bool ParseUnsigned(const std::string& strValue, std::uint64_t& value)
	{
		if (strValue.empty())
		{
			return false;
		}
		const char* pBegin = strValue.data();
		const char* pEnd = pBegin + strValue.size();
		std::from_chars_result result = std::from_chars(pBegin, pEnd, value);
		return (std::errc() == result.ec) && (pEnd == result.ptr);
	}

	bool ParseParameters(const std::string& strJson, CStrategyConfig& config)
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
			config.m_parameters.emplace(std::string(mIter->name.GetString(), mIter->name.GetStringLength()), std::move(strValue));
		}
		return true;
	}

	bool ParseSubscriptions(const std::string& strJson, CStrategyConfig& config)
	{
		rapidjson::Document document;
		document.Parse(strJson.c_str());
		if (document.HasParseError() || !document.IsArray())
		{
			return false;
		}
		config.m_subscriptions.reserve(document.Size());
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
			config.m_subscriptions.emplace_back(std::move(quote));
		}
		return !config.m_subscriptions.empty();
	}

	bool ParseFlag(const std::string& strValue, bool& bValue)
	{
		if (("1" == strValue) || ("true" == strValue))
		{
			bValue = true;
			return true;
		}
		if (("0" == strValue) || ("false" == strValue))
		{
			bValue = false;
			return true;
		}
		return false;
	}

	bool MakeStrategyConfig(const request::StrategyInfo& info, CStrategyConfig& cfg)
	{
		cfg.m_strategyId = info.strategy_id();
		cfg.m_accountIndex = AccountIndex(info);
		cfg.m_strType = info.strategy_type();
		cfg.m_strName = info.strategy_name();
		cfg.m_eventQueueLimit = (0 == info.event_queue_limit()) ? 4096 : info.event_queue_limit();
		cfg.m_bAutoStart = info.auto_start();
		for (const auto& [strKey, strValue] : info.parameters())
		{
			cfg.m_parameters.emplace(strKey, strValue);
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
		return cfg.IsValid();
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
		bool bAutoStart = false;
		bool bEnabled = false;
		CStrategyConfig cfg;
		if ((9 != row.size()) || !ParseUnsigned(row[0], id) || !ParseUnsigned(row[5], queueLimit) || !ParseFlag(row[6], bAutoStart) || !ParseFlag(row[7], bEnabled) || !ParseParameters(row[3], cfg) || !ParseSubscriptions(row[4], cfg))
		{
			return false;
		}
		if (!DecodeAccount(row[8], cfg.m_accountIndex))
		{
			return false;
		}
		SetAccountIdentity(cfg.m_accountIndex, info);
		info.set_strategy_id(id);
		info.set_strategy_type(row[1]);
		info.set_strategy_name(row[2]);
		info.set_event_queue_limit(static_cast<std::uint32_t>(queueLimit));
		info.set_auto_start(bAutoStart);
		info.set_enabled(bEnabled);
		for (const auto& [strKey, strValue] : cfg.m_parameters)
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
	std::atomic_uint64_t s_id{ 1 };
	m_sequence = s_id.fetch_add(1, std::memory_order_relaxed);
}

bool CSignalKey::operator==(const CSignalKey& arg) const
{
	return (m_strategyId == arg.m_strategyId) && (m_signalId == arg.m_signalId);
}

std::size_t CSignalKeyHash::operator()(const CSignalKey& key) const
{
	std::size_t strategyHash = std::hash<_TyStrategyId>{}(key.m_strategyId);
	std::size_t signalHash = std::hash<_TySignalId>{}(key.m_signalId);
	return strategyHash ^ (signalHash + 0x9e3779b9U + (strategyHash << 6) + (strategyHash >> 2));
}

CStrategyService::CStrategyService(CSession* pSession, CTradeService* pTradeService) : m_pSession(pSession), m_pTradeService(pTradeService)
{
	m_bMarketAvailable.store((nullptr != m_pSession) && m_pSession->IsAuthenticated());
}

CStrategyService::~CStrategyService()
{
	StopAll();
	if (nullptr != m_pTradeService)
	{
		m_pTradeService->SetOrderEventHandler({});
		m_pTradeService->SetTradeEventHandler({});
		m_pTradeService->SetAccountReadyHandler({});
	}
}

bool CStrategyService::Initialize()
{
	m_strLastError.clear();
	if (nullptr == m_pSession || nullptr == m_pTradeService)
	{
		m_strLastError = "Strategy session or trade service is unavailable";
		return false;
	}
	m_pTradeService->SetOrderEventHandler(std::bind_front(&CStrategyService::OnOrderEvent, this));
	m_pTradeService->SetTradeEventHandler(std::bind_front(&CStrategyService::OnTradeEvent, this));
	m_pTradeService->SetAccountReadyHandler(std::bind_front(&CStrategyService::OnAccountReady, this));
	m_pSession->RegisterHandler(std::bind_front(&CStrategyService::OnHQMarketResponse, this));
	m_pSession->RegisterStateHandler(std::bind_front(&CStrategyService::OnHQMarketState, this));
	return LoadStrategies();
}

void CStrategyService::OnAccountReady(const _TyAccount& accountIndex)
{
	if (m_bStopping.load())
	{
		return;
	}
	std::vector<_TyStrategyId> deferred;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_strategies);
		for (const auto& item : m_runtimes)
		{
			if (accountIndex == item.second->m_config.m_accountIndex && item.second->m_config.m_bAutoStart && StrategyState::Initialized == item.second->m_state.load())
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

bool CStrategyService::HandleStrategyRequest(const CRequest& req)
{
	std::lock_guard<std::mutex> operationLock(m_mtx_operations);
	if (!req.IsStrategy())
	{
		net::SendError(req.GetConnectionId(), req, 1101, "invalid strategy request type");
		return false;
	}
	_TyRequest type = req.GetType();
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
	if (!IsAccountValid(AccountIndex(info)))
	{
		net::SendError(req.GetConnectionId(), req, 1202, "Account identity required");
		return false;
	}
	if (!m_pTradeService->HasAccount(AccountIndex(info)))
	{
		net::SendError(req.GetConnectionId(), req, 1114, "Trading account does not exist");
		return false;
	}
	info.set_strategy_id(1);
	if (0 == info.event_queue_limit())
	{
		info.set_event_queue_limit(4096);
	}
	CStrategyConfig cfg;
	if (!MakeStrategyConfig(info, cfg) || (nullptr == CreateStrategy(info.strategy_type())))
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
	std::string strSQL = "INSERT INTO table_strategy(strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,enabled,account_identity) VALUES(" + utility::Utf8Literal(info.strategy_type()) + "," + utility::Utf8Literal(info.strategy_name()) + "," + utility::Utf8Literal(SerializeParameters(info)) + "," + utility::Utf8Literal(SerializeSubscriptions(info)) + "," + std::to_string(info.event_queue_limit()) + "," + (info.auto_start() ? "1" : "0") + "," + (info.enabled() ? "1" : "0") + "," + utility::Utf8Literal(EncodeAccount(AccountIndex(info))) + ")";
	if (0 != pDB->ExecUpdate(strSQL))
	{
		net::SendError(req.GetConnectionId(), req, 1105, "failed to add strategy");
		return false;
	}
	const db::CTable& idTable = pDB->ExecQuery("SELECT LAST_INSERT_ID()");
	std::uint64_t id = 0;
	if (idTable.m_rows.empty() || idTable.m_rows.front().empty() || !ParseUnsigned(db::to_string(idTable.m_rows.front().front()), id))
	{
		net::SendError(req.GetConnectionId(), req, 1106, "failed to obtain strategy id");
		return false;
	}
	cfg.m_strategyId = id;
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
	if (!IsAccountValid(AccountIndex(info)))
	{
		db::_TyDBPtr lookup = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
		if (nullptr == lookup)
		{
			net::SendError(req.GetConnectionId(), req, 1104, "strategy database is unavailable");
			return false;
		}
		const db::CTable& account = lookup->ExecQuery("SELECT account_identity FROM table_strategy WHERE strategy_id=" + std::to_string(info.strategy_id()));
		_TyAccount accountIndex;
		if (account.m_rows.empty() || account.m_rows.front().empty() || !DecodeAccount(db::to_string(account.m_rows.front().front()), accountIndex))
		{
			net::SendError(req.GetConnectionId(), req, 1110, "strategy does not exist");
			return false;
		}
		SetAccountIdentity(accountIndex, info);
	}
	if (!m_pTradeService->HasAccount(AccountIndex(info)))
	{
		net::SendError(req.GetConnectionId(), req, 1114, "Trading account does not exist");
		return false;
	}
	if (0 == info.event_queue_limit())
	{
		info.set_event_queue_limit(4096);
	}
	CStrategyConfig cfg;
	if (!MakeStrategyConfig(info, cfg) || (nullptr == CreateStrategy(info.strategy_type())))
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
	const db::CTable& existing = pDB->ExecQuery("SELECT strategy_id FROM table_strategy WHERE strategy_id=" + std::to_string(cfg.m_strategyId));
	if (existing.m_rows.empty())
	{
		net::SendError(req.GetConnectionId(), req, 1110, "strategy does not exist");
		return false;
	}
	std::shared_ptr<CStrategyRuntime> oldRuntime = FindRuntime(cfg.m_strategyId);
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
		if ((oldConfig.m_accountIndex != cfg.m_accountIndex) && ((StrategyState::Running == oldState) || (StrategyState::Starting == oldState) || (StrategyState::Pausing == oldState) || (StrategyState::Stopping == oldState) || ownActiveOrders))
		{
			net::SendError(req.GetConnectionId(), req, 1115, "Stop strategy and resolve orders before rebinding account");
			return false;
		}
		if (!StopStrategy(cfg.m_strategyId) || !RemoveStrategy(cfg.m_strategyId))
		{
			net::SendError(req.GetConnectionId(), req, 1108, "strategy runtime cannot be replaced");
			return false;
		}
	}
	if (info.enabled() && !CreateStrategy(cfg))
	{
		if (nullptr != oldRuntime)
		{
			CreateStrategy(oldConfig);
		}
		net::SendError(req.GetConnectionId(), req, 1107, "failed to create strategy runtime");
		return false;
	}
	std::string strSQL = "UPDATE table_strategy SET strategy_type=" + utility::Utf8Literal(info.strategy_type()) + ",strategy_name=" + utility::Utf8Literal(info.strategy_name()) + ",parameters=" + utility::Utf8Literal(SerializeParameters(info)) + ",subscriptions=" + utility::Utf8Literal(SerializeSubscriptions(info)) + ",event_queue_limit=" + std::to_string(info.event_queue_limit()) + ",auto_start=" + (info.auto_start() ? "1" : "0") + ",enabled=" + (info.enabled() ? "1" : "0") + ",account_identity=" + utility::Utf8Literal(EncodeAccount(AccountIndex(info))) + " WHERE strategy_id=" + std::to_string(cfg.m_strategyId);
	if (0 != pDB->ExecUpdate(strSQL))
	{
		if (info.enabled())
		{
			StopStrategy(cfg.m_strategyId);
			RemoveStrategy(cfg.m_strategyId);
		}
		if (nullptr != oldRuntime)
		{
			CreateStrategy(oldConfig);
		}
		net::SendError(req.GetConnectionId(), req, 1111, "failed to modify strategy");
		return false;
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
	const db::CTable& table = pDB->ExecQuery("SELECT strategy_id,strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,enabled,account_identity FROM table_strategy ORDER BY strategy_id");
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
	if (!ParseUnsigned(req.GetExtraData("strategy_id"), id) || (0 == id))
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
		pRecord->set_strategy_id(snapshot.m_strategyId);
		pRecord->set_state(static_cast<int>(snapshot.m_state));
		pRecord->set_market_available(snapshot.m_bMarketAvailable);
		pRecord->set_queue_length(snapshot.m_queueLength);
		pRecord->set_active_orders(snapshot.m_activeOrderCount);
		pRecord->set_last_error(snapshot.m_strLastError);
	}
	response.SetData(list);
	return net::SendRequest(req.GetConnectionId(), response);
}

bool CStrategyService::ControlStrategy(const CRequest& req)
{
	std::uint64_t id = 0;
	if (!ParseUnsigned(req.GetExtraData("strategy_id"), id) || (0 == id))
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
	if ((_TyRequest::STRATEGY_START == type) && (nullptr != runtime) && !m_pTradeService->IsConnected(runtime->m_config.m_accountIndex))
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

bool CStrategyService::LoadStrategies()
{
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		m_strLastError = "Strategy database is unavailable";
		return false;
	}
	const db::CTable& tableExists = pDB->ExecQuery("SHOW TABLES LIKE 'table_strategy'");
	if (tableExists.m_rows.empty())
	{
		m_strLastError = "MySQL table_strategy does not exist";
		return false;
	}
	const db::CTable& table = pDB->ExecQuery("SELECT strategy_id,strategy_type,strategy_name,parameters,subscriptions,event_queue_limit,auto_start,account_identity FROM table_strategy WHERE enabled=1 ORDER BY strategy_id");
	for (const db::_TyRow& values : table.m_rows)
	{
		std::vector<std::string> row;
		row.reserve(values.size());
		for (const db::_TyCell& value : values)
		{
			row.emplace_back(db::to_string(value));
		}
		if (8 != row.size())
		{
			m_strLastError = "table_strategy contains an invalid row";
			return false;
		}
		CStrategyConfig cfg;
		std::uint64_t id = 0;
		std::uint64_t eventQueueLimit = 0;
		std::uint64_t autoStart = 0;
		if (!ParseUnsigned(row[0], id) || !ParseUnsigned(row[5], eventQueueLimit) || !ParseUnsigned(row[6], autoStart))
		{
			m_strLastError = "table_strategy contains invalid numeric fields";
			return false;
		}
		cfg.m_strategyId = id;
		cfg.m_strType = row[1];
		cfg.m_strName = row[2];
		cfg.m_eventQueueLimit = static_cast<std::size_t>(eventQueueLimit);
		cfg.m_bAutoStart = 0 != autoStart;
		if (!DecodeAccount(row[7], cfg.m_accountIndex))
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

std::unique_ptr<IStrategy> CStrategyService::CreateStrategy(const std::string& strType) const
{
	if ("moving_average" == strType)
	{
		return std::make_unique<CMovingAverageStrategy>();
	}
	return nullptr;
}

bool CStrategyService::CreateStrategy(const CStrategyConfig& cfg)
{
	if (!cfg.IsValid() || m_bStopping.load())
	{
		return false;
	}
	std::unique_ptr<IStrategy> strategy = CreateStrategy(cfg.m_strType);
	if (nullptr == strategy)
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = std::make_shared<CStrategyRuntime>();
	runtime->m_config = cfg;
	runtime->m_strategy = std::move(strategy);
	runtime->m_context = std::make_unique<CStrategyContext>(cfg.m_strategyId, cfg.m_accountIndex, this, m_pTradeService);
	if (!runtime->m_strategy->Initialize(*runtime->m_context, runtime->m_config))
	{
		return false;
	}
	runtime->m_state.store(StrategyState::Initialized);

	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_strategies);
		if (!m_runtimes.emplace(cfg.m_strategyId, runtime).second)
		{
			return false;
		}
	}

	for (const auto& order : m_pTradeService->GetOrders(cfg.m_accountIndex))
	{
		if (cfg.m_strategyId != order.m_strategyId)
		{
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(m_mtx_signals);
			m_signals[{ cfg.m_strategyId, order.m_signalId }] = SignalState::Accepted;
		}
		if (trade::IsActive(order.m_status))
		{
			{
				std::lock_guard<std::mutex> lock(runtime->m_mtx_orders);
				runtime->m_activeOrderIds.emplace(order.m_orderId);
			}
			{
				std::unique_lock<std::shared_mutex> lock(m_mtx_orders);
				m_orderRoutes[{ order.m_accountIndex, order.m_orderId }] = cfg.m_strategyId;
				m_clientOrderRoutes[{ order.m_accountIndex, order.m_clientId }] = cfg.m_strategyId;
			}
		}
	}
	NotifySnapshot(runtime);
	if (cfg.m_bAutoStart && m_pTradeService->IsConnected(cfg.m_accountIndex) && !StartStrategy(cfg.m_strategyId))
	{
		RemoveStrategy(cfg.m_strategyId);
		return false;
	}
	return true;
}

bool CStrategyService::RemoveStrategy(_TyStrategyId id)
{
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
	UnsubscribeUnusedQuotes(runtime->m_config);
	WaitUntilIdle(runtime);
	runtime->m_context->Disable();
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_strategies);
		m_runtimes.erase(id);
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_signals);
		std::erase_if(m_signals, [id](const auto& item)
		{
			return id == item.first.m_strategyId;
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
	if (m_bStopping.load() || nullptr == m_pTradeService)
	{
		return false;
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if (nullptr == runtime)
	{
		return false;
	}
	StrategyState state = runtime->m_state.load();
	if (((StrategyState::Initialized != state) && (StrategyState::Paused != state) && (StrategyState::Stopped != state)) || !m_pTradeService->BeginStrategy(runtime->m_config.m_accountIndex, id))
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
		m_pTradeService->EndStrategy(runtime->m_config.m_accountIndex, id);
		return false;
	}
	if ((StrategyState::Initialized == state) || (StrategyState::Stopped == state))
	{
		AddRoutes(runtime->m_config);
		SubscribeRequiredQuotes(runtime->m_config);
	}
	if (!ExecuteControl(id, StrategyEventType::Start))
	{
		runtime->m_state.store(state);
		m_pTradeService->EndStrategy(runtime->m_config.m_accountIndex, id);
		RemoveRoutes(runtime->m_config);
		UnsubscribeUnusedQuotes(runtime->m_config);
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
	m_pTradeService->EndStrategy(runtime->m_config.m_accountIndex, id);
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
	UnsubscribeUnusedQuotes(runtime->m_config);
	if (bStopped)
	{
		m_pTradeService->EndStrategy(runtime->m_config.m_accountIndex, id);
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
			StopStrategy(runtime->m_config.m_strategyId);
		}
		RemoveRoutes(runtime->m_config);
		UnsubscribeUnusedQuotes(runtime->m_config);
	}
	for (const auto& runtime : runtimes)
	{
		WaitUntilIdle(runtime);
		runtime->m_context->Disable();
	}
}

void CStrategyService::OnHQMarketResponse(const CRequest& req)
{
	if (m_bStopping.load() || !m_bMarketAvailable.load() || (!req.IsMarket()))
	{
		return;
	}
	std::string strKey = GetMarketKey(req);
	if (strKey.empty())
	{
		return;
	}
	const _TyReqData& data = req.GetData();
	CStrategyEvent ev;
	if (data.has_quote())
	{
		ev.m_type = StrategyEventType::Quote;
		ev.m_data = data.quote();
	}
	else if (data.has_depth())
	{
		ev.m_type = StrategyEventType::Depth;
		ev.m_data = data.depth();
	}
	else if (data.has_trade())
	{
		ev.m_type = StrategyEventType::MarketTrade;
		ev.m_data = data.trade();
	}
	else if (data.has_bar())
	{
		ev.m_type = StrategyEventType::Bar;
		ev.m_data = data.bar();
	}
	else
	{
		return;
	}
	RouteMarketEvent(strKey, std::move(ev));
}

void CStrategyService::OnHQMarketState(SessionState state, const std::string& strReason)
{
	bool bAvailable = SessionState::Ready == state;
	m_bMarketAvailable.store(bAvailable);
	if (bAvailable)
	{
		RestoreRequiredSubscriptions();
	}
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
	_TyStrategyId id = ev.m_strategyId;
	if (0 == id)
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_orders);
		const auto mIter = m_orderRoutes.find({ ev.m_accountIndex, ev.m_orderId });
		if (m_orderRoutes.end() != mIter)
		{
			id = mIter->second;
		}
		else
		{
			const auto clientIter = m_clientOrderRoutes.find({ ev.m_accountIndex, ev.m_clientOrderId });
			if (m_clientOrderRoutes.end() != clientIter)
			{
				id = clientIter->second;
			}
		}
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (ev.m_accountIndex != runtime->m_config.m_accountIndex))
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
		m_orderRoutes[{ ev.m_accountIndex, ev.m_orderId }] = id;
		m_clientOrderRoutes[{ ev.m_accountIndex, ev.m_clientOrderId }] = id;
	}
	// Keep correlation through terminal order reports: a broker may deliver its fill afterward.

	CStrategyEvent strategyEvent;
	strategyEvent.m_type = StrategyEventType::Order;
	strategyEvent.m_data = ev;
	EnqueueEvent(runtime, std::move(strategyEvent));
}

void CStrategyService::OnTradeEvent(const CTradeEvent& ev)
{
	_TyStrategyId id = ev.m_strategyId;
	if (0 == id)
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_orders);
		const auto mIter = m_orderRoutes.find({ ev.m_accountIndex, ev.m_orderId });
		if (m_orderRoutes.end() != mIter)
		{
			id = mIter->second;
		}
	}
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (ev.m_accountIndex != runtime->m_config.m_accountIndex))
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
	currentStrategyId = runtime.m_config.m_strategyId;
	try
	{
		switch (event.m_type)
		{
		case StrategyEventType::Start:
			runtime.m_strategy->OnStart();
			runtime.m_state.store(StrategyState::Running);
			runtime.m_bAcceptMarketEvents.store(true);
			break;
		case StrategyEventType::Pause:
			runtime.m_strategy->OnPause();
			runtime.m_state.store(StrategyState::Paused);
			break;
		case StrategyEventType::Stop:
			runtime.m_strategy->OnStop();
			runtime.m_state.store(StrategyState::Stopped);
			break;
		case StrategyEventType::Quote:
			runtime.m_strategy->OnQuote(std::get<_TyQuoteData>(event.m_data));
			break;
		case StrategyEventType::Depth:
			runtime.m_strategy->OnDepth(std::get<_TyDepthData>(event.m_data));
			break;
		case StrategyEventType::MarketTrade:
			runtime.m_strategy->OnMarketTrade(std::get<_TyMarketTradeData>(event.m_data));
			break;
		case StrategyEventType::Bar:
			runtime.m_strategy->OnBar(std::get<_TyBarData>(event.m_data));
			break;
		case StrategyEventType::Order:
			runtime.m_strategy->OnOrder(std::get<COrderEvent>(event.m_data));
			break;
		case StrategyEventType::Trade:
			runtime.m_strategy->OnTrade(std::get<CTradeEvent>(event.m_data));
			break;
		case StrategyEventType::Timer:
			runtime.m_strategy->OnTimer(std::get<CTimerEvent>(event.m_data));
			break;
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
	std::shared_ptr<CStrategyRuntime> runtimeHandle = FindRuntime(runtime.m_config.m_strategyId);
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
	m_pTradeService->EndStrategy(runtime.m_config.m_accountIndex, runtime.m_config.m_strategyId);
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
	for (const auto& runtime : runtimes)
	{
		CStrategyEvent routedEvent = ev;
		EnqueueEvent(runtime, std::move(routedEvent));
	}
}

void CStrategyService::AddRoutes(const CStrategyConfig& cfg)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
	for (const auto& quote : cfg.m_subscriptions)
	{
		std::string strKey = quote.String();
		m_marketRoutes[strKey].emplace(cfg.m_strategyId);
		CSubscriptionEntry& entry = m_subscriptions[strKey];
		entry.m_quote = quote;
		entry.m_strategyIds.emplace(cfg.m_strategyId);
	}
}

void CStrategyService::RemoveRoutes(const CStrategyConfig& cfg)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
	for (const auto& quote : cfg.m_subscriptions)
	{
		std::string strKey = quote.String();
		const auto routeIter = m_marketRoutes.find(strKey);
		if (m_marketRoutes.end() != routeIter)
		{
			routeIter->second.erase(cfg.m_strategyId);
			if (routeIter->second.empty())
			{
				m_marketRoutes.erase(routeIter);
			}
		}
		const auto subscriptionIter = m_subscriptions.find(strKey);
		if (m_subscriptions.end() != subscriptionIter)
		{
			subscriptionIter->second.m_strategyIds.erase(cfg.m_strategyId);
		}
	}
}

void CStrategyService::SubscribeRequiredQuotes(const CStrategyConfig& cfg)
{
	if ((nullptr == m_pSession) || !m_bMarketAvailable.load())
	{
		return;
	}
	std::vector<CQuoteInfo> quotes;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
		quotes.reserve(cfg.m_subscriptions.size());
		for (const auto& quote : cfg.m_subscriptions)
		{
			const auto mIter = m_subscriptions.find(quote.String());
			if ((m_subscriptions.end() != mIter) && !mIter->second.m_bSubscribed && !mIter->second.m_bSubscribing)
			{
				mIter->second.m_bSubscribing = true;
				quotes.emplace_back(quote);
			}
		}
	}
	for (const auto& quote : quotes)
	{
		bool bSubscribed = m_pSession->SendRequest(request::Subscription(quote));
		std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
		const auto mIter = m_subscriptions.find(quote.String());
		if (m_subscriptions.end() != mIter)
		{
			mIter->second.m_bSubscribed = bSubscribed;
			mIter->second.m_bSubscribing = false;
		}
	}
}

void CStrategyService::UnsubscribeUnusedQuotes(const CStrategyConfig& cfg)
{
	std::vector<CQuoteInfo> quotes;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
		for (const auto& quote : cfg.m_subscriptions)
		{
			const auto mIter = m_subscriptions.find(quote.String());
			if ((m_subscriptions.end() != mIter) && mIter->second.m_strategyIds.empty())
			{
				if (mIter->second.m_bSubscribed)
				{
					quotes.emplace_back(mIter->second.m_quote);
				}
				m_subscriptions.erase(mIter);
			}
		}
	}
	if (nullptr != m_pSession)
	{
		for (const auto& quote : quotes)
		{
			m_pSession->SendRequest(request::UnSubscription(quote));
		}
	}
}

void CStrategyService::RestoreRequiredSubscriptions()
{
	if (nullptr == m_pSession)
	{
		return;
	}
	std::vector<CQuoteInfo> quotes;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
		quotes.reserve(m_subscriptions.size());
		for (const auto& item : m_subscriptions)
		{
			if (!item.second.m_strategyIds.empty() && !item.second.m_bSubscribed && !item.second.m_bSubscribing)
			{
				m_subscriptions[item.first].m_bSubscribing = true;
				quotes.emplace_back(item.second.m_quote);
			}
		}
	}
	for (const auto& quote : quotes)
	{
		bool bSubscribed = m_pSession->SendRequest(request::Subscription(quote));
		std::unique_lock<std::shared_mutex> lock(m_mtx_routes);
		const auto mIter = m_subscriptions.find(quote.String());
		if (m_subscriptions.end() != mIter)
		{
			mIter->second.m_bSubscribed = bSubscribed;
			mIter->second.m_bSubscribing = false;
		}
	}
}

COrderSubmitResult CStrategyService::SubmitOrder(_TyStrategyId id, const COrderIntent& intent)
{
	std::shared_ptr<CStrategyRuntime> runtime = FindRuntime(id);
	if ((nullptr == runtime) || (nullptr == m_pTradeService) || (StrategyState::Running != runtime->m_state.load()) || !intent.IsValid())
	{
		return { false, false, 0, 0, "Strategy cannot submit this order" };
	}
	if (!m_bMarketAvailable.load() && ((PositionEffect::Open == intent.m_positionEffect) || (PositionEffect::Unknown == intent.m_positionEffect)))
	{
		return { false, true, 0, 0, "Market is unavailable for opening orders" };
	}
	CSignalKey key{ id, intent.m_signalId };
	{
		std::lock_guard<std::mutex> lock(m_mtx_signals);
		if (!m_signals.emplace(key, SignalState::Pending).second)
		{
			return { false, false, 0, 0, "Duplicate strategy signal" };
		}
	}
	COrderIntent routedIntent = intent;
	routedIntent.m_accountIndex = runtime->m_config.m_accountIndex;
	COrderSubmitResult result = m_pTradeService->Submit(routedIntent);
	{
		std::lock_guard<std::mutex> lock(m_mtx_signals);
		if (!result.m_bAccepted && result.m_bRetryable)
		{
			m_signals.erase(key);
		}
		else
		{
			if (!result.m_bAccepted)
			{
				m_signals[key] = SignalState::Rejected;
			}
		}
	}

	return result;
}

bool CStrategyService::CancelOrder(_TyStrategyId id, _TyOrderId orderId)
{
	if ((nullptr == m_pTradeService) || (0 == orderId))
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
		const auto mIter = m_orderRoutes.find({ runtime->m_config.m_accountIndex, orderId });
		if ((m_orderRoutes.end() == mIter) || (id != mIter->second))
		{
			return false;
		}
	}
	return m_pTradeService->Cancel(id, orderId);
}

CStrategySnapshot CStrategyService::MakeSnapshot(const std::shared_ptr<CStrategyRuntime>& runtime) const
{
	CStrategySnapshot snapshot;
	snapshot.m_strategyId = runtime->m_config.m_strategyId;
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
