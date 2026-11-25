#include "../log/CLogger.h"
#include "CTradeService.h"
#include "../trade/ExecutionRulesWire.h"
#include "TradeRequestRouting.h"
#include <iostream>
#include "../network/CNetTools.h"
#include "../request/request.h"
#include <optional>
#include <algorithm>
#include <unordered_set>
#include "../common/container.h"
#include "../trade/TradeProtocol.h"

CTradeService::CTradeService(CAccountService* pAccountService, CSession* pSession) : m_account_service(pAccountService), m_session(pSession)
{
}

CTradeService::~CTradeService()
{
	Stop();
}

bool CTradeService::Initialize()
{
	if (nullptr == m_account_service || nullptr == m_session)
	{
		m_last_error = "Account service is unavailable";
		return false;
	}
	m_last_error.clear();
	m_account_service->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, this));
	m_running.store(true);
	m_refresh = std::jthread([this](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
			RefreshPending();
			RefreshTrades();
			std::unique_lock<std::mutex> lock(m_wait_mutex);
			m_wake.wait_for(lock, std::chrono::seconds(1), [&stop]()
			{
				return stop.stop_requested();
			});
		}
	});
	return true;
}

void CTradeService::Stop()
{
	if (!m_running.exchange(false))
	{
		return;
	}
	m_refresh.request_stop();
	m_wake.notify_all();
	if (m_refresh.joinable())
	{
		m_refresh.join();
	}
	m_account_service->SetAccountStateHandler({});
	OnBrokerState(SessionState::Stopping);
}

const std::string& CTradeService::GetLastError() const
{
	return m_last_error;
}

void CTradeService::SetStrategyReplyHandler(_TyStrategyReplyHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_handlers_mutex);
	m_strategy_reply_handler = std::move(handler);
}

trade::CResult CTradeService::SendStrategyRequest(CRequest&& request)
{
	_TyAccount account;
	if (!m_running.load() || !FmtAccount(request, account) || !IsReady(account))
	{
		return { trade::Error::Unavailable, "Account not synchronized" };
	}
	if (_TyRequest::TRADE_ORDER_SUBMIT != request.GetType() && _TyRequest::TRADE_ORDER_CANCEL != request.GetType())
	{
		return { trade::Error::Unsupported, "Unsupported strategy request" };
	}
	std::uint64_t strategyId = 0;
	std::uint64_t instanceId = 0;
	if (!tradingroute::ReadNumber(request, "strategy_id", strategyId) || 0 == strategyId || !tradingroute::ReadNumber(request, "strategy_instance_id", instanceId) || 0 == instanceId)
	{
		return { trade::Error::NotFound, "Strategy routing identity required" };
	}
	request.SetExtraData("request_source", "Strategy");
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		request.SetExtraData("broker_connect_id", std::to_string(m_generation));
	}
	return SendRequest(std::move(request)) ? trade::CResult{} : trade::CResult{ trade::Error::Unavailable, "XBroker is unavailable" };
}

bool CTradeService::OnRequest(const CRequest& req)
{
	if (!m_running.load())
	{
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "Trade service is unavailable");
		return false;
	}
	if (_TyRequest::TRADE_ORDER_SUBMIT == req.GetType() || _TyRequest::TRADE_ORDER_CANCEL == req.GetType())
	{
		_TyAccount account;
		if (!FmtAccount(req, account) || !IsReady(account))
		{
			net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker account is not synchronized");
			return false;
		}
	}
	CRequest request = req;
	request.SetExtraData("request_source", "Client");
	request.SetExtraData("broker_connect_id", "");
	request.SetExtraData("sync_cycle", "");
	request.SetExtraData("strategy_instance_id", "");
	bool bQueued = SendRequest(std::move(request));
	if (!bQueued)
	{
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker is unavailable");
	}
	return bQueued;
}

bool CTradeService::HasActiveOrders(const _TyAccount& account) const
{
	if (!m_account_service->IsConnected(account))
	{
		return false;
	}
	for (const auto& order : Snapshot(account).m_orders)
	{
		if (trade::IsActive(order.m_status))
		{
			return true;
		}
	}
	return false;
}

CAccountSnapshot CTradeService::GetAccount(const _TyAccount& account) const
{
	return GetStrategyAccountView(account, {}).m_account;
}

CPositionSnapshot CTradeService::GetPosition(const _TyAccount& account, const CSecurity& security) const
{
	return GetStrategyAccountView(account, security).m_position;
}

CStrategyAccountView CTradeService::GetStrategyAccountView(const _TyAccount& account, const CSecurity& security) const
{
	return MakeStrategyAccountView(Snapshot(account), security);
}

std::vector<trade::COrder> CTradeService::GetOrders(const _TyAccount& account) const
{
	return m_account_service->HasAccount(account) ? Snapshot(account).m_orders : std::vector<trade::COrder>{};
}

void CTradeService::QueryAccount(const _TyAccount& account)
{
	if (!m_account_service->IsConnected(account))
	{
		return;
	}
	std::uint64_t nGeneration = 0;
	std::uint64_t nCycle = m_next_sync_id.fetch_add(1);
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		if (m_snapshots[account].m_ready || container::with(m_syncing, account))
		{
			return;
		}
		nGeneration = m_generation;
		m_syncing[account].m_cycle = nCycle;
	}
	QueryNext(account, nGeneration, nCycle, trade::Operation::Funds);
}

void CTradeService::QueryNext(const _TyAccount& account, std::uint64_t nGeneration, std::uint64_t nCycle, trade::Operation operation)
{
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		const auto pending = m_syncing.find(account);
		if (nGeneration != m_generation || m_syncing.end() == pending || nCycle != pending->second.m_cycle)
		{
			return;
		}
	}
	trade::CReqParam request;
	request.m_account = account;
	switch (operation)
	{
	case trade::Operation::Funds:
		request.m_param = trade::CFundsQueryParam{};
		break;
	case trade::Operation::Positions:
		request.m_param = trade::CPositionsQueryParam{};
		break;
	case trade::Operation::Orders:
		request.m_param = trade::COrdersQueryParam{};
		break;
	case trade::Operation::Trades:
		request.m_param = trade::CDealsQueryParam{};
		break;
	default:
		return;
	}
	CRequest wire = trade::protocol::WriteRequest(request);
	wire.SetExtraData("request_source", "AccountSync");
	wire.SetExtraData("broker_connect_id", std::to_string(nGeneration));
	wire.SetExtraData("sync_cycle", std::to_string(nCycle));
	trade::CResult result = SendRequest(std::move(wire)) ? trade::CResult{} : trade::CResult{ trade::Error::Unavailable, "XBroker is unavailable" };
	if (!result.Ok())
	{
		trade::CCounterReply reply;
		reply.m_account = account;
		reply.m_operation = operation;
		reply.m_result = std::move(result);
		CompleteQuery(reply, nGeneration, nCycle);
	}
}

void CTradeService::CompleteQuery(const trade::CCounterReply& reply, std::uint64_t nGeneration, std::uint64_t nCycle)
{
	_TyAccount account = reply.m_account;
	std::vector<trade::CCounterEvent> notifications;
	trade::Operation next = trade::Operation::Funds;
	bool bNext = false;
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		const auto pending = m_syncing.find(account);
		if (nGeneration != m_generation || m_syncing.end() == pending || nCycle != pending->second.m_cycle)
		{
			return;
		}
		if (!m_running.load() || !m_account_service->IsConnected(account) || !reply.m_result.Ok() || pending->second.m_overflow)
		{
			m_snapshots[account].m_ready = false;
			m_syncing.erase(pending);
			return;
		}
		trade::CAccountData& data = pending->second.m_data;
		std::size_t nCategory = static_cast<std::size_t>(reply.m_operation) - static_cast<std::size_t>(trade::Operation::Funds);
		if (4 <= nCategory || nCategory != pending->second.m_completed)
		{
			m_syncing.erase(pending);
			return;
		}
		// Each category has its own query watermark; these are not an atomic snapshot.
		data.m_version = std::max(data.m_version, reply.m_version);
		data.m_time = std::max(data.m_time, reply.m_time);
		switch (reply.m_operation)
		{
		case trade::Operation::Funds:
			data.m_account = std::get<trade::CFunds>(reply.m_data);
			data.m_funds_version = reply.m_version;
			break;
		case trade::Operation::Positions:
			data.m_positions = std::get<std::vector<trade::CPosition>>(reply.m_data);
			data.m_positions_version = reply.m_version;
			break;
		case trade::Operation::Orders:
			data.m_orders = std::get<std::vector<trade::COrder>>(reply.m_data);
			data.m_orders_version = reply.m_version;
			break;
		case trade::Operation::Trades:
			data.m_tradesVersion = reply.m_version;
			data.m_trades = std::get<std::vector<trade::CTrade>>(reply.m_data);
			break;
		default:
			break;
		}
		++pending->second.m_completed;
		if (4 != pending->second.m_completed)
		{
			next = static_cast<trade::Operation>(static_cast<int>(reply.m_operation) + 1);
			bNext = true;
		}
		else
		{
			trade::CAccountData& cached = m_snapshots[account];
			std::unordered_set<std::uint64_t> previousTrades;
			previousTrades.reserve(cached.m_trades.size());
			for (const auto& row : cached.m_trades)
			{
				previousTrades.emplace(row.m_trade_id);
			}
			cached = std::move(pending->second.m_data);
			// Reconcile complete state before the final session-ready notification.
			for (const auto& order : cached.m_orders)
			{
				trade::CCounterEvent ev;
				ev.m_account = account;
				ev.m_kind = trade::EventTy::Order;
				ev.m_order = order;
				ev.m_funds = cached.m_account;
				notifications.emplace_back(std::move(ev));
			}
			for (const auto& row : cached.m_trades)
			{
				if (!previousTrades.emplace(row.m_trade_id).second)
				{
					continue;
				}
				trade::CCounterEvent ev;
				ev.m_account = account;
				ev.m_kind = trade::EventTy::Deal;
				ev.m_trade = row;
				ev.m_funds = cached.m_account;
				for (const auto& order : cached.m_orders)
				{
					if (row.m_order_id == order.m_order_id)
					{
						ev.m_order = order;
						break;
					}
				}
				notifications.emplace_back(std::move(ev));
			}
			std::stable_sort(pending->second.m_events.begin(), pending->second.m_events.end(), [](const trade::CCounterEvent& lhs, const trade::CCounterEvent& rhs)
			{
				return lhs.m_sequence < rhs.m_sequence;
			});
			for (auto& ev : pending->second.m_events)
			{
				std::uint64_t fundsVersion = cached.m_funds_version;
				if (0 == ev.m_sequence || !ApplyEvent(cached, ev))
				{
					continue;
				}
				if (trade::EventTy::Deal == ev.m_kind && !previousTrades.emplace(ev.m_trade.m_trade_id).second)
				{
					if (fundsVersion == cached.m_funds_version)
					{
						continue;
					}
					ev.m_kind = trade::EventTy::Funds;
				}
				notifications.emplace_back(std::move(ev));
			}
			cached.m_ready = m_account_service->IsConnected(account) && cached.m_account.m_connected && cached.m_account.m_funds_valid;
			trade::CCounterEvent session;
			session.m_account = account;
			session.m_kind = trade::EventTy::Session;
			session.m_funds = cached.m_account;
			notifications.emplace_back(session);
			m_syncing.erase(pending);
		}
	}
	if (bNext)
	{
		QueryNext(account, nGeneration, nCycle, next);
	}
	for (const auto& ev : notifications)
	{
		PublishEvent(ev);
	}
}

trade::CAccountData CTradeService::Snapshot(const _TyAccount& account) const
{
	std::lock_guard<std::mutex> lock(m_state_mutex);
	const auto mIter = m_snapshots.find(account);
	if (!IsAccountValid(account) || m_snapshots.end() == mIter)
	{
		trade::CAccountData data;
		data.m_account.m_account = account;
		return data;
	}
	trade::CAccountData data = mIter->second;
	const auto epochIter = m_account_epochs.find(account);
	data.m_connection_epoch = m_account_epochs.end() == epochIter ? 0 : epochIter->second;
	data.m_ready = data.m_ready && m_running.load() && m_account_service->IsConnected(account);
	return data;
}

bool CTradeService::ApplyEvent(trade::CAccountData& data, trade::CCounterEvent& ev)
{
	std::uint64_t* categoryVersion = &data.m_funds_version;
	switch (ev.m_kind)
	{
	case trade::EventTy::Order:
		categoryVersion = &data.m_orders_version;
		break;
	case trade::EventTy::Position:
		categoryVersion = &data.m_positions_version;
		break;
	case trade::EventTy::Deal:
		categoryVersion = &data.m_tradesVersion;
		break;
	default:
		break;
	}
	// A push can advance its row category and funds independently.
	bool newFunds = ev.m_account_valid && (0 == ev.m_sequence || data.m_funds_version < ev.m_sequence);
	bool newCategory = trade::EventTy::Funds != ev.m_kind && (0 == ev.m_sequence || *categoryVersion < ev.m_sequence);
	if (!newFunds && !newCategory)
	{
		return false;
	}
	if (newCategory)
	{
		*categoryVersion = (std::max)(*categoryVersion, ev.m_sequence);
	}
	else
	{
		ev.m_kind = trade::EventTy::Funds;
	}
	if (newFunds)
	{
		data.m_funds_version = (std::max)(data.m_funds_version, ev.m_sequence);
	}
	else
	{
		ev.m_funds = data.m_account;
	}
	ev.m_account_valid = true;
	if (trade::EventTy::Deal == ev.m_kind)
	{
		for (const auto& order : data.m_orders)
		{
			if (ev.m_trade.m_order_id == order.m_order_id)
			{
				ev.m_order = order;
				break;
			}
		}
	}
	std::uint64_t previous = data.m_version;
	trade::UpdateAccountData(data, ev);
	data.m_version = (std::max)(previous, ev.m_sequence);
	return true;
}

void CTradeService::OnBrokerEvent(trade::CCounterEvent ev)
{
	if (!m_running.load())
	{
		return;
	}
	if (trade::EventTy::Session == ev.m_kind)
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		m_syncing.erase(ev.m_account);
		trade::CAccountData& data = m_snapshots[ev.m_account];
		data.m_ready = false;
		data.m_account.m_connected = ev.m_funds.m_connected;
		data.m_account.m_funds_valid = false;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		const auto pending = m_syncing.find(ev.m_account);
		if (m_syncing.end() != pending)
		{
			if (4096 <= pending->second.m_events.size())
			{
				pending->second.m_overflow = true;
			}
			else
			{
				pending->second.m_events.emplace_back(ev);
			}
			return;
		}
		const auto mIter = m_snapshots.find(ev.m_account);
		if (m_snapshots.end() == mIter || !mIter->second.m_ready || !ApplyEvent(mIter->second, ev))
		{
			return;
		}
	}
	PublishEvent(ev);
}

bool CTradeService::IsReady(const _TyAccount& account) const
{
	return m_running.load() && GetAccount(account).m_ready;
}

void CTradeService::SetEventHandler(_TyEventHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_handlers_mutex);
	m_event_handler = std::move(handler);
}

void CTradeService::SetPushHandler(_TyRequestNotify&& handler)
{
	std::lock_guard<std::mutex> lock(m_handlers_mutex);
	m_push_handler = std::move(handler);
}

void CTradeService::SetAccountReadyHandler(_TyAccountReadyHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_handlers_mutex);
	m_account_ready_handler = std::move(handler);
}

void CTradeService::PublishEvent(const trade::CCounterEvent& ev)
{
	_TyEventHandler eventHandler;
	_TyRequestNotify pushHandler;
	_TyAccountReadyHandler readyHandler;
	{
		std::lock_guard<std::mutex> lock(m_handlers_mutex);
		eventHandler = m_event_handler;
		pushHandler = m_push_handler;
		readyHandler = m_account_ready_handler;
	}
	if (nullptr != eventHandler)
	{
		eventHandler(ev);
	}
	if (trade::EventTy::Session == ev.m_kind)
	{
		if (IsReady(ev.m_account) && nullptr != readyHandler)
		{
			readyHandler(ev.m_account);
		}
	}
	else if (nullptr != pushHandler)
	{
		pushHandler(trade::protocol::Push(ev, {}));
	}
}

void CTradeService::OnBrokerState(SessionState state, const std::string&)
{
	if (SessionState::Ready == state)
	{
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		++m_generation;
		m_rules.clear();
		m_syncing.clear();
		for (auto& entry : m_snapshots)
		{
			m_account_epochs[entry.first] = ++m_next_account_epoch;
			entry.second.m_ready = false;
			entry.second.m_account.m_connected = false;
			entry.second.m_account.m_funds_valid = false;
		}
	}
	FailPending();
}

void CTradeService::RefreshTrades()
{
	std::vector<_TyAccount> accounts = m_account_service->GetAccounts();
	std::set<_TyAccount> connected;
	for (const auto& account : accounts)
	{
		if (m_account_service->IsConnected(account))
		{
			connected.emplace(account);
		}
	}
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		std::erase_if(m_snapshots, [&accounts](const auto& entry)
		{
			return !container::with(accounts, entry.first);
		});
		std::erase_if(m_syncing, [&connected](const auto& entry)
		{
			return !container::with(connected, entry.first);
		});
		for (auto& entry : m_snapshots)
		{
			if (!container::with(connected, entry.first))
			{
				entry.second.m_ready = false;
				entry.second.m_account.m_funds_valid = false;
			}
		}
	}
	if (!m_session->IsAuthenticated())
	{
		return;
	}
	for (const auto& account : accounts)
	{
		QueryAccount(account);
	}
}

void CTradeService::OnAccountState(const _TyAccount& account)
{
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		m_account_epochs[account] = ++m_next_account_epoch;
		std::erase_if(m_rules, [&account](const auto& item)
		{
			return item.first.first == account;
		});
		m_syncing.erase(account);
		const auto mIter = m_snapshots.find(account);
		if (m_snapshots.end() != mIter)
		{
			mIter->second.m_ready = false;
			mIter->second.m_account.m_funds_valid = false;
			if (!m_account_service->HasAccount(account))
			{
				m_snapshots.erase(mIter);
			}
		}
	}
}

bool CTradeService::SendRequest(CRequest&& request)
{
	if ((!m_running.load()) || !m_session->IsAuthenticated())
	{
		return false;
	}
	std::uint64_t nId = CRequest().GetId();
	CRequest origin = request;
	request.SetExtraData("source_request_id", std::to_string(request.GetId()));
	request.SetId(nId);
	{
		std::lock_guard<std::mutex> lock(m_requests_mutex);
		if (!m_session->IsAuthenticated() || !m_running.load())
		{
			return false;
		}
		if (4096 <= m_pending.size())
		{
			return false;
		}
		m_pending.emplace(nId, CPending{ std::move(origin), std::chrono::steady_clock::now() + std::chrono::seconds(_TyRequest::TRADE_ACCOUNT_LOGIN == request.GetType() ? 60 : 15) });
	}
	if (!m_session->SendRequest(request))
	{
		// A write failure may follow a partial write. Complete as uncertain, never retry.
		CRequest response;
		response.SetId(nId);
		response.SetType(request.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker write failed; reconcile before retry");
		OnReply(response);
	}
	return true;
}

bool CTradeService::OnReply(const CRequest& req)
{
	if (!m_running.load())
	{
		return false;
	}
	if (0 == req.GetId())
	{
		trade::CCounterEvent ev;
		if (!trade::protocol::ReadPush(req, ev))
		{
			return false;
		}
		OnBrokerEvent(std::move(ev));
		return true;
	}
	CPending pending;
	{
		std::lock_guard<std::mutex> lock(m_requests_mutex);
		const auto mIter = m_pending.find(req.GetId());
		if (m_pending.end() == mIter || req.GetType() != mIter->second.m_origin.GetType())
		{
			return false;
		}
		pending = std::move(mIter->second);
		m_pending.erase(mIter);
	}
	return DispatchReply(pending.m_origin, req);
}

bool CTradeService::DispatchReply(const CRequest& origin, const CRequest& response)
{
	std::string source = origin.GetExtraData("request_source");
	if ("ExecutionRules" == source)
	{
		_TyAccount account;
		std::uint64_t generation = 0;
		if (!FmtAccount(origin, account) || !tradingroute::ReadNumber(origin, "broker_connect_id", generation))
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(m_state_mutex);
		const auto mIter = m_rules.find({ account, origin.GetExtraData("security") });
		if ((generation != m_generation) || (m_rules.end() == mIter) || (origin.GetId() != mIter->second.m_request_id))
		{
			return false;
		}
		mIter->second.m_request_id = 0;
		mIter->second.m_retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		std::optional<std::pair<int, std::string>> error = response.GetErrorInfo();
		if ((!error.has_value() || (0 == error->first)) && response.GetData().has_execution_rules() && (origin.GetExtraData("security") == response.GetData().execution_rules().security()))
		{
			mIter->second.m_value = trade::ReadExecutionRules(response.GetData().execution_rules());
		}
		return true;
	}
	if ("Client" == source)
	{
		CRequest reply = response;
		reply.SetId(origin.GetId());
		return net::SendRequest(origin.GetConnectionId(), reply);
	}
	if ("Strategy" != source && "AccountSync" != source)
	{
		CLogger::Write(Level::Error, "strategy", "Invalid trade request source");
		return false;
	}
	std::uint64_t generation = 0;
	std::uint64_t cycle = 0;
	_TyAccount account;
	if (!tradingroute::ReadNumber(origin, "broker_connect_id", generation) || !FmtAccount(origin, account) || ("AccountSync" == source && !tradingroute::ReadNumber(origin, "sync_cycle", cycle)))
	{
		CLogger::Write(Level::Error, "strategy", "Invalid trade request routing metadata");
		return false;
	}
	trade::Operation operation;
	switch (origin.GetType())
	{
	case _TyRequest::TRADE_ORDER_SUBMIT:
		operation = trade::Operation::Submit;
		break;
	case _TyRequest::TRADE_ORDER_CANCEL:
		operation = trade::Operation::Cancel;
		break;
	case _TyRequest::TRADE_FUNDS_QUERY:
		operation = trade::Operation::Funds;
		break;
	case _TyRequest::TRADE_POSITION_QUERY:
		operation = trade::Operation::Positions;
		break;
	case _TyRequest::TRADE_ORDER_QUERY:
		operation = trade::Operation::Orders;
		break;
	case _TyRequest::TRADE_DEAL_QUERY:
		operation = trade::Operation::Trades;
		break;
	default:
		return false;
	}
	if (("Strategy" == source && trade::Operation::Submit != operation && trade::Operation::Cancel != operation) || ("AccountSync" == source && (trade::Operation::Funds > operation || trade::Operation::Trades < operation)))
	{
		CLogger::Write(Level::Error, "strategy", "Invalid trade request source/operation");
		return false;
	}
	trade::CCounterReply reply;
	if (!trade::protocol::ReadReply(response, reply) || (reply.m_result.Ok() && (account != reply.m_account || operation != reply.m_operation)))
	{
		reply.m_result = { trade::Error::Unavailable, "Invalid XBroker response" };
		reply.m_data = std::monostate{};
	}
	reply.m_account = account;
	reply.m_request_id = origin.GetId();
	reply.m_operation = operation;
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		if (generation != m_generation)
		{
			reply.m_result = { trade::Error::UnknownResult, "Stale XBroker connection" };
			reply.m_data = std::monostate{};
		}
	}
	if ("AccountSync" == source)
	{
		CompleteQuery(reply, generation, cycle);
		return true;
	}
	_TyStrategyReplyHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_handlers_mutex);
		handler = m_strategy_reply_handler;
	}
	if (nullptr != handler)
	{
		handler(origin, reply);
	}
	return true;
}

void CTradeService::FailPending()
{
	std::unordered_map<_TyRequestId, CPending> pending;
	{
		std::lock_guard<std::mutex> lock(m_requests_mutex);
		pending.swap(m_pending);
	}
	for (const auto& item : pending)
	{
		CRequest response;
		response.SetId(item.first);
		response.SetType(item.second.m_origin.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker disconnected; reconcile before retry");
		DispatchReply(item.second.m_origin, response);
	}
}

void CTradeService::RefreshPending()
{
	std::vector<std::pair<std::uint64_t, _TyRequest>> expired;
	{
		std::lock_guard<std::mutex> lock(m_requests_mutex);
		for (const auto& item : m_pending)
		{
			if (item.second.m_deadline <= std::chrono::steady_clock::now())
			{
				expired.emplace_back(item.first, item.second.m_origin.GetType());
			}
		}
	}
	for (const auto& item : expired)
	{
		CRequest response;
		response.SetId(item.first);
		response.SetType(item.second);
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker request timed out; reconcile before retry");
		OnReply(response);
	}
}

std::optional<trade::CExecutionRule> CTradeService::GetExecutionRules(const _TyAccount& account, const CSecurity& security)
{
	if (!IsReady(account))
	{
		return std::nullopt;
	}
	std::string securityKey = security.String();
	CRequest req;
	req.SetType(_TyRequest::TRADE_EXECUTION_RULES_QUERY);
	WriteAccountIdentity(account, req);
	req.SetExtraData("security", securityKey);
	req.SetExtraData("request_source", "ExecutionRules");
	_TyRequestId requestId = req.GetId();
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		CRuleCache& cache = m_rules[{ account, securityKey }];
		std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		if (cache.m_value.has_value() && (now < cache.m_value->m_calendar.m_close_time_ms))
		{
			return cache.m_value;
		}
		cache.m_value.reset();
		if ((0 != cache.m_request_id) || (std::chrono::steady_clock::now() < cache.m_retry))
		{
			return std::nullopt;
		}
		cache.m_request_id = req.GetId();
		req.SetExtraData("broker_connect_id", std::to_string(m_generation));
	}
	if (!SendRequest(std::move(req)))
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		const auto mIter = m_rules.find({ account, securityKey });
		if ((m_rules.end() != mIter) && (requestId == mIter->second.m_request_id))
		{
			mIter->second.m_request_id = 0;
			mIter->second.m_retry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		}
	}
	return std::nullopt;
}

void CTradeService::ReconcileAccount(const _TyAccount& account)
{
	{
		std::lock_guard<std::mutex> lock(m_state_mutex);
		m_syncing.erase(account);
		m_snapshots[account].m_ready = false;
	}
	QueryAccount(account);
}
