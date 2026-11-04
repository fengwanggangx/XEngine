#include "CTradeService.h"
#include "TradeRequestRouting.h"
#include <iostream>
#include "../network/CNetTools.h"
#include "../request/request.h"
#include <optional>
#include <algorithm>
#include <unordered_set>
#include "../common/container.h"
#include "../trade/TradeProtocol.h"

CTradeService::CTradeService(CAccountService* pAccountService, CSession* pSession) : m_pAccountService(pAccountService), m_pSession(pSession)
{
}

CTradeService::~CTradeService()
{
	Stop();
}

bool CTradeService::Initialize()
{
	if (nullptr == m_pAccountService || nullptr == m_pSession)
	{
		m_strLastError = "Account service is unavailable";
		return false;
	}
	m_strLastError.clear();
	m_pAccountService->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, this));
	m_bRunning.store(true);
	m_pSession->RegisterHandler(std::bind_front(&CTradeService::OnRequestReply, this));
	m_pSession->RegisterStateHandler(std::bind_front(&CTradeService::OnBrokerState, this));
	m_refresh = std::jthread([this](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
			RefreshPending();
			RefreshTrades();
			std::unique_lock<std::mutex> lock(m_mtx_wait);
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
	if (!m_bRunning.exchange(false))
	{
		return;
	}
	m_refresh.request_stop();
	m_wake.notify_all();
	if (m_refresh.joinable())
	{
		m_refresh.join();
	}
	m_pAccountService->SetAccountStateHandler({});
	OnBrokerState(SessionState::Stopping);
}

const std::string& CTradeService::GetLastError() const
{
	return m_strLastError;
}

void CTradeService::SetStrategyReplyHandler(_TyStrategyReplyHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_strategyReplyHandler = std::move(handler);
}

trade::CResult CTradeService::SendStrategyRequest(CRequest&& request)
{
	_TyAccount account;
	if (!m_bRunning.load() || !FmtAccount(request, account) || !IsReady(account))
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
		std::lock_guard<std::mutex> lock(m_mtx_state);
		request.SetExtraData("broker_connect_id", std::to_string(m_generation));
	}
	return SendRequest(std::move(request)) ? trade::CResult{} : trade::CResult{ trade::Error::Unavailable, "XBroker is unavailable" };
}

bool CTradeService::HandleTradeRequest(const CRequest& req)
{
	if (!m_bRunning.load())
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
	if (!m_pAccountService->IsConnected(account))
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
	CAccountSnapshot result;
	if (!m_pAccountService->IsConnected(account))
	{
		return result;
	}
	trade::CAccountData snapshot = Snapshot(account);
	result.m_bReady = snapshot.m_bReady;
	result.m_version = snapshot.m_version;
	result.m_nTime = snapshot.m_nTime;
	if (!IsAccountValid(snapshot.m_account.m_account) || !snapshot.m_account.m_bConnected)
	{
		result.m_bReady = false;
		return result;
	}
	result.m_balance = snapshot.m_account.m_nBalance;
	result.m_frozen = snapshot.m_account.m_nFrozen;
	result.m_available = snapshot.m_account.m_bExplicitAvailable ? snapshot.m_account.m_nAvailable : result.m_balance - result.m_frozen;
	result.m_bReady = result.m_bReady && snapshot.m_account.m_bFundsValid;
	return result;
}

CPositionSnapshot CTradeService::GetPosition(const _TyAccount& account, const CSecurity& security) const
{
	if (m_pAccountService->IsConnected(account))
	{
		trade::CAccountData state = Snapshot(account);
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		snapshot.m_bReady = GetAccount(account).m_bReady;
		snapshot.m_version = state.m_version;
		snapshot.m_nTime = state.m_nTime;
		for (const auto& position : state.m_positions)
		{
			if (security.String() == position.m_strSecurity)
			{
				snapshot.m_totalQuantity = position.m_nTotal;
				snapshot.m_availableQuantity = position.m_nAvailable;
				snapshot.m_frozenQuantity = position.m_nFrozen;
				snapshot.m_averageCost = 0 == position.m_nTotal ? 0 : position.m_nCost / position.m_nTotal;
			}
		}
		return snapshot;
	}
	CPositionSnapshot snapshot;
	snapshot.m_security = security;
	return snapshot;
}

std::vector<trade::COrder> CTradeService::GetOrders(const _TyAccount& account) const
{
	return m_pAccountService->HasAccount(account) ? Snapshot(account).m_orders : std::vector<trade::COrder>{};
}

void CTradeService::QueryAccount(const _TyAccount& account)
{
	if (!m_pAccountService->IsConnected(account))
	{
		return;
	}
	std::uint64_t nGeneration = 0;
	std::uint64_t nCycle = m_nNextSyncId.fetch_add(1);
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (m_snapshots[account].m_bReady || m_syncing.contains(account))
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
		std::lock_guard<std::mutex> lock(m_mtx_state);
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
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto pending = m_syncing.find(account);
		if (nGeneration != m_generation || m_syncing.end() == pending || nCycle != pending->second.m_cycle)
		{
			return;
		}
		if (!m_bRunning.load() || !m_pAccountService->IsConnected(account) || !reply.m_result.Ok() || pending->second.m_bOverflow)
		{
			m_snapshots[account].m_bReady = false;
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
		pending->second.m_versions[nCategory] = reply.m_version;
		if (0 == nCategory)
		{
			pending->second.m_versions[4] = reply.m_version;
		}
		data.m_version = std::max(data.m_version, reply.m_version);
		data.m_nTime = std::max(data.m_nTime, reply.m_nTime);
		switch (reply.m_operation)
		{
		case trade::Operation::Funds:
			data.m_account = std::get<trade::CFunds>(reply.m_data);
			break;
		case trade::Operation::Positions:
			data.m_positions = std::get<std::vector<trade::CPosition>>(reply.m_data);
			break;
		case trade::Operation::Orders:
			data.m_orders = std::get<std::vector<trade::COrder>>(reply.m_data);
			break;
		case trade::Operation::Trades:
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
				previousTrades.emplace(row.m_tradeId);
			}
			cached = std::move(pending->second.m_data);
			cached.m_bReady = m_pAccountService->IsConnected(account) && cached.m_account.m_bConnected && cached.m_account.m_bFundsValid;
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
				if (previousTrades.contains(row.m_tradeId))
				{
					continue;
				}
				trade::CCounterEvent ev;
				event.m_account = account;
				event.m_kind = trade::EventTy::Deal;
				event.m_trade = row;
				event.m_funds = cached.m_account;
				for (const auto& order : cached.m_orders)
				{
					if (row.m_orderId == order.m_orderId)
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
				std::size_t nCategory = 0;
				switch (ev.m_kind)
				{
				case trade::EventTy::Position:
					nCategory = 1;
					break;
				case trade::EventTy::Order:
					nCategory = 2;
					break;
				case trade::EventTy::Deal:
					nCategory = 3;
					break;
				case trade::EventTy::Session:
					nCategory = 4;
					break;
				default:
					break;
				}
				if (pending->second.m_versions[nCategory] < ev.m_sequence)
				{
					std::uint64_t nVersion = cached.m_version;
					ApplyEvent(cached, ev);
					cached.m_version = std::max(nVersion, cached.m_version);
					pending->second.m_versions[nCategory] = ev.m_sequence;
					notifications.emplace_back(ev);
				}
			}
			trade::CCounterEvent session;
			session.m_account = account;
			session.m_kind = trade::EventTy::Session;
			session.m_account = cached.m_account;
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
	std::lock_guard<std::mutex> lock(m_mtx_state);
	const auto mIter = m_snapshots.find(account);
	if (!IsAccountValid(account) || m_snapshots.end() == mIter)
	{
		trade::CAccountData data;
		data.m_account.m_account = account;
		return data;
	}
	trade::CAccountData data = mIter->second;
	data.m_bReady = data.m_bReady && m_bRunning.load() && m_pAccountService->IsConnected(account);
	return data;
}

void CTradeService::ApplyEvent(trade::CAccountData& data, trade::CCounterEvent& ev)
{
	if (trade::EventTy::Funds != ev.m_kind && !ev.m_bAccountValid)
	{
		bool bConnected = ev.m_funds.m_bConnected;
		int state = ev.m_funds.m_nState;
		ev.m_funds = data.m_account;
		if (trade::EventTy::Session == ev.m_kind)
		{
			ev.m_funds.m_bConnected = bConnected;
			ev.m_funds.m_nState = state;
			if (!bConnected)
			{
				ev.m_funds.m_bFundsValid = false;
			}
		}
		ev.m_bAccountValid = true;
	}
	if (trade::EventTy::Deal == ev.m_kind)
	{
		for (const auto& order : data.m_orders)
		{
			if (ev.m_trade.m_orderId == order.m_orderId)
			{
				ev.m_order = order;
				break;
			}
		}
	}
	std::uint64_t previous = data.m_version;
	trade::UpdateAccountData(data, event);
	if (0 == ev.m_sequence)
	{
		data.m_version = previous;
	}
}

void CTradeService::OnBrokerEvent(trade::CCounterEvent ev)
{
	if (!m_bRunning.load())
	{
		return;
	}
	if (trade::EventTy::Session == ev.m_kind)
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_syncing.erase(ev.m_account);
		trade::CAccountData& data = m_snapshots[ev.m_account];
		data.m_bReady = false;
		data.m_account.m_bConnected = ev.m_funds.m_bConnected;
		data.m_account.m_bFundsValid = false;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		auto pending = m_syncing.find(ev.m_account);
		if (m_syncing.end() != pending)
		{
			if (4096 <= pending->second.m_events.size())
			{
				pending->second.m_bOverflow = true;
			}
			else
			{
				pending->second.m_events.emplace_back(ev);
			}
			return;
		}
		auto iter = m_snapshots.find(ev.m_account);
		if (m_snapshots.end() == iter || !iter->second.m_bReady || (0 != ev.m_sequence && ev.m_sequence <= iter->second.m_version))
		{
			return;
		}
		ApplyEvent(iter->second, ev);
	}
	PublishEvent(ev);
}

bool CTradeService::IsReady(const _TyAccount& account) const
{
	return m_bRunning.load() && GetAccount(account).m_bReady;
}

void CTradeService::SetEventHandler(_TyEventHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_eventHandler = std::move(handler);
}

void CTradeService::SetPushHandler(_TyRequestNotify&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_pushHandler = std::move(handler);
}

void CTradeService::SetAccountReadyHandler(_TyAccountReadyHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_accountReadyHandler = std::move(handler);
}

void CTradeService::PublishEvent(const trade::CCounterEvent& ev)
{
	_TyEventHandler eventHandler;
	_TyRequestNotify pushHandler;
	_TyAccountReadyHandler readyHandler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		if (0 != ev.m_sequence && ev.m_sequence <= m_lastEventSequence[ev.m_account])
		{
			return;
		}
		if (0 != ev.m_sequence)
		{
			m_lastEventSequence[ev.m_account] = ev.m_sequence;
		}
		eventHandler = m_eventHandler;
		pushHandler = m_pushHandler;
		readyHandler = m_accountReadyHandler;
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
		std::lock_guard<std::mutex> lock(m_mtx_state);
		++m_generation;
		m_syncing.clear();
		for (auto& entry : m_snapshots)
		{
			entry.second.m_bReady = false;
			entry.second.m_account.m_bConnected = false;
			entry.second.m_account.m_bFundsValid = false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		m_lastEventSequence.clear();
	}
	FailPending();
}

void CTradeService::RefreshTrades()
{
	std::vector<_TyAccount> accounts = m_pAccountService->GetAccounts();
	std::set<_TyAccount> connected;
	for (const auto& account : accounts)
	{
		if (m_pAccountService->IsConnected(account))
		{
			connected.emplace(account);
		}
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
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
				entry.second.m_bReady = false;
				entry.second.m_account.m_bFundsValid = false;
			}
		}
	}
	if (!m_pSession->IsAuthenticated())
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
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_syncing.erase(account);
		const auto mIter = m_snapshots.find(account);
		if (m_snapshots.end() != mIter)
		{
			mIter->second.m_bReady = false;
			mIter->second.m_account.m_bFundsValid = false;
			if (!m_pAccountService->HasAccount(account))
			{
				m_snapshots.erase(mIter);
			}
		}
	}
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_lastEventSequence.erase(account);
}

bool CTradeService::SendRequest(CRequest&& request)
{
	if ((!m_bRunning.load()) || !m_pSession->IsAuthenticated())
	{
		return false;
	}
	std::uint64_t nId = CRequest().GetId();
	CRequest origin = request;
	request.SetExtraData("source_request_id", std::to_string(request.GetId()));
	request.SetId(nId);
	{
		std::lock_guard<std::mutex> lock(m_mtx_requests);
		if (!m_pSession->IsAuthenticated() || !m_bRunning.load())
		{
			return false;
		}
		if (4096 <= m_pending.size())
		{
			return false;
		}
		m_pending.emplace(nId, CPending{ std::move(origin), std::chrono::steady_clock::now() + std::chrono::seconds(_TyRequest::TRADE_ACCOUNT_LOGIN == request.GetType() ? 60 : 15) });
	}
	if (!m_pSession->SendRequest(request))
	{
		// A write failure may follow a partial write. Complete as uncertain, never retry.
		CRequest response;
		response.SetId(nId);
		response.SetType(request.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker write failed; reconcile before retry");
		OnRequestReply(response);
	}
	return true;
}

bool CTradeService::OnRequestReply(const CRequest& req)
{
	if (!m_bRunning.load())
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
		std::lock_guard<std::mutex> lock(m_mtx_requests);
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
	if ("Client" == source)
	{
		CRequest reply = response;
		reply.SetId(origin.GetId());
		return net::SendRequest(origin.GetConnectionId(), reply);
	}
	if ("Strategy" != source && "AccountSync" != source)
	{
		std::cerr << "Invalid trade request source\n";
		return false;
	}
	std::uint64_t generation = 0;
	std::uint64_t cycle = 0;
	_TyAccount account;
	if (!tradingroute::ReadNumber(origin, "broker_connect_id", generation) || !FmtAccount(origin, account) || ("AccountSync" == source && !tradingroute::ReadNumber(origin, "sync_cycle", cycle)))
	{
		std::cerr << "Invalid trade request routing metadata\n";
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
		std::cerr << "Invalid trade request source/operation\n";
		return false;
	}
	trade::CCounterReply reply;
	if (!trade::protocol::ReadReply(response, reply) || (reply.m_result.Ok() && (account != reply.m_account || operation != reply.m_operation)))
	{
		reply.m_result = { trade::Error::Unavailable, "Invalid XBroker response" };
		reply.m_data = std::monostate{};
	}
	reply.m_account = account;
	reply.m_requestId = origin.GetId();
	reply.m_operation = operation;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
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
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		handler = m_strategyReplyHandler;
	}
	if (nullptr != handler)
	{
		handler(origin, reply);
	}
	return true;
}

void CTradeService::FailPending()
{
	std::unordered_map<std::uint64_t, CPending> pending;
	{
		std::lock_guard<std::mutex> lock(m_mtx_requests);
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
		std::lock_guard<std::mutex> lock(m_mtx_requests);
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
		OnRequestReply(response);
	}
}
