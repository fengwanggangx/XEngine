#include "CTradeService.h"
#include "../network/CNetTools.h"
#include "../request/request.h"
#include <optional>
#include <algorithm>
#include <unordered_set>
#include "../common/container.h"
#include "../trade/TradeProtocol.h"

CTradeService::CTradeService(CAccountService* pAccountService, CBrokerClient* pBroker) : m_pAccountService(pAccountService), m_pBroker(pBroker)
{
}

CTradeService::~CTradeService()
{
	Stop();
}

bool CTradeService::Initialize()
{
	if (nullptr == m_pAccountService || nullptr == m_pBroker)
	{
		m_strLastError = "Account service is unavailable";
		return false;
	}
	m_strLastError.clear();
	m_pAccountService->SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, this));
	m_bRunning.store(true);
	m_refresh = std::jthread([this](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
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

trade::CResult CTradeService::SendRequest(trade::CReqParam request, CBrokerClient::_TyReplyHandler handler)
{
	if (!m_bRunning.load() || !IsReady(request.m_accountIndex))
	{
		return { trade::Error::Unavailable, "Account not synchronized" };
	}
	std::optional<trade::COrder> order;
	if (const trade::CSubmitParam* parameters = std::get_if<trade::CSubmitParam>(&request.m_param); nullptr != parameters)
	{
		order = parameters->m_order;
		order->m_accountIndex = request.m_accountIndex;
	}
	return m_pBroker->SendRequest(std::move(request), [this, order = std::move(order), handler = std::move(handler)](const trade::CCounterReply& reply)
	{
		if (order.has_value() && ((trade::Error::Rejected == reply.m_result.m_error) || (trade::Error::Unsupported == reply.m_result.m_error) || (trade::Error::Conflict == reply.m_result.m_error) || (trade::Error::NotFound == reply.m_result.m_error)))
		{
			trade::CCountEvent event;
			event.m_kind = trade::EventTy::Order;
			event.m_accountIndex = order->m_accountIndex;
			event.m_order = *order;
			event.m_order.m_status = trade::Status::Rejected;
			PublishEvent(event);
		}
		if (nullptr != handler)
		{
			handler(reply);
		}
	});
}

bool CTradeService::HandleTradingRequest(const CRequest& req)
{
	if (!m_bRunning.load())
	{
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "Trade service is unavailable");
		return false;
	}
	if (_TyRequest::TRADE_ORDER_SUBMIT == req.GetType() || _TyRequest::TRADE_ORDER_CANCEL == req.GetType())
	{
		_TyAccount accountIndex;
		if (!ReadAccountIdentity(req, accountIndex) || !IsReady(accountIndex))
		{
			net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker account is not synchronized");
			return false;
		}
	}
	CRequest request = req;
	bool bQueued = m_pBroker->Forward(std::move(request), [original = req](const CRequest& reply)
	{
		CRequest response = reply;
		response.SetId(original.GetId());
		net::SendRequest(original.GetConnectionId(), response);
	});
	if (!bQueued)
	{
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker is unavailable");
	}
	return bQueued;
}

bool CTradeService::HasActiveOrders(const _TyAccount& accountIndex) const
{
	if (!m_pAccountService->IsConnected(accountIndex))
	{
		return false;
	}
	for (const auto& order : Snapshot(accountIndex).m_orders)
	{
		if (trade::IsActive(order.m_status))
		{
			return true;
		}
	}
	return false;
}

CAccountSnapshot CTradeService::GetAccount(const _TyAccount& accountIndex) const
{
	CAccountSnapshot result;
	if (!m_pAccountService->IsConnected(accountIndex))
	{
		return result;
	}
	trade::CAccountData snapshot = Snapshot(accountIndex);
	result.m_bReady = snapshot.m_bReady;
	result.m_version = snapshot.m_version;
	result.m_nTime = snapshot.m_nTime;
	if (!IsAccountValid(snapshot.m_account.m_accountIndex) || !snapshot.m_account.m_bConnected)
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

CPositionSnapshot CTradeService::GetPosition(const _TyAccount& accountIndex, const CSecurity& security) const
{
	if (m_pAccountService->IsConnected(accountIndex))
	{
		trade::CAccountData state = Snapshot(accountIndex);
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		snapshot.m_bReady = GetAccount(accountIndex).m_bReady;
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

std::vector<trade::COrder> CTradeService::GetOrders(const _TyAccount& accountIndex) const
{
	return m_pAccountService->HasAccount(accountIndex) ? Snapshot(accountIndex).m_orders : std::vector<trade::COrder>{};
}

void CTradeService::QueryAccount(const _TyAccount& accountIndex)
{
	if (!m_pAccountService->IsConnected(accountIndex))
	{
		return;
	}
	std::uint64_t nGeneration = 0;
	std::uint64_t nCycle = m_nNextSyncId.fetch_add(1);
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (m_snapshots[accountIndex].m_bReady || m_syncing.contains(accountIndex))
		{
			return;
		}
		nGeneration = m_generation;
		m_syncing[accountIndex].m_cycle = nCycle;
	}
	QueryNext(accountIndex, nGeneration, nCycle, trade::Operation::Funds);
}

void CTradeService::QueryNext(const _TyAccount& accountIndex, std::uint64_t nGeneration, std::uint64_t nCycle, trade::Operation operation)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto pending = m_syncing.find(accountIndex);
		if (nGeneration != m_generation || m_syncing.end() == pending || nCycle != pending->second.m_cycle)
		{
			return;
		}
	}
	trade::CReqParam request;
	request.m_accountIndex = accountIndex;
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
	trade::CResult result = m_pBroker->SendRequest(std::move(request), [this, nGeneration, nCycle](const trade::CCounterReply& reply)
	{
		CompleteQuery(reply, nGeneration, nCycle);
	});
	if (!result.Ok())
	{
		trade::CCounterReply reply;
		reply.m_accountIndex = accountIndex;
		reply.m_operation = operation;
		reply.m_result = std::move(result);
		CompleteQuery(reply, nGeneration, nCycle);
	}
}

void CTradeService::CompleteQuery(const trade::CCounterReply& reply, std::uint64_t nGeneration, std::uint64_t nCycle)
{
	_TyAccount accountIndex = reply.m_accountIndex;
	std::vector<trade::CCountEvent> notifications;
	trade::Operation next = trade::Operation::Funds;
	bool bNext = false;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto pending = m_syncing.find(accountIndex);
		if (nGeneration != m_generation || m_syncing.end() == pending || nCycle != pending->second.m_cycle)
		{
			return;
		}
		if (!m_bRunning.load() || !m_pAccountService->IsConnected(accountIndex) || !reply.m_result.Ok() || pending->second.m_bOverflow)
		{
			m_snapshots[accountIndex].m_bReady = false;
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
			data.m_account = std::get<trade::CAccount>(reply.m_data);
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
			trade::CAccountData& cached = m_snapshots[accountIndex];
			std::unordered_set<std::uint64_t> previousTrades;
			previousTrades.reserve(cached.m_trades.size());
			for (const auto& row : cached.m_trades)
			{
				previousTrades.emplace(row.m_tradeId);
			}
			cached = std::move(pending->second.m_data);
			cached.m_bReady = m_pAccountService->IsConnected(accountIndex) && cached.m_account.m_bConnected && cached.m_account.m_bFundsValid;
			// Reconcile complete state before the final session-ready notification.
			for (const auto& order : cached.m_orders)
			{
				trade::CCountEvent event;
				event.m_accountIndex = accountIndex;
				event.m_kind = trade::EventTy::Order;
				event.m_order = order;
				event.m_account = cached.m_account;
				notifications.emplace_back(std::move(event));
			}
			for (const auto& row : cached.m_trades)
			{
				if (previousTrades.contains(row.m_tradeId))
				{
					continue;
				}
				trade::CCountEvent event;
				event.m_accountIndex = accountIndex;
				event.m_kind = trade::EventTy::Deal;
				event.m_trade = row;
				event.m_account = cached.m_account;
				for (const auto& order : cached.m_orders)
				{
					if (row.m_orderId == order.m_orderId)
					{
						event.m_order = order;
						break;
					}
				}
				notifications.emplace_back(std::move(event));
			}
			std::stable_sort(pending->second.m_events.begin(), pending->second.m_events.end(), [](const trade::CCountEvent& lhs, const trade::CCountEvent& rhs)
			{
				return lhs.m_sequence < rhs.m_sequence;
			});
			for (auto& event : pending->second.m_events)
			{
				std::size_t nCategory = 0;
				switch (event.m_kind)
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
				if (pending->second.m_versions[nCategory] < event.m_sequence)
				{
					std::uint64_t nVersion = cached.m_version;
					ApplyEvent(cached, event);
					cached.m_version = std::max(nVersion, cached.m_version);
					pending->second.m_versions[nCategory] = event.m_sequence;
					notifications.emplace_back(event);
				}
			}
			trade::CCountEvent session;
			session.m_accountIndex = accountIndex;
			session.m_kind = trade::EventTy::Session;
			session.m_account = cached.m_account;
			notifications.emplace_back(session);
			m_syncing.erase(pending);
		}
	}
	if (bNext)
	{
		QueryNext(accountIndex, nGeneration, nCycle, next);
	}
	for (const auto& event : notifications)
	{
		PublishEvent(event);
	}
}

trade::CAccountData CTradeService::Snapshot(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	const auto mIter = m_snapshots.find(accountIndex);
	if (!IsAccountValid(accountIndex) || m_snapshots.end() == mIter)
	{
		trade::CAccountData data;
		data.m_account.m_accountIndex = accountIndex;
		return data;
	}
	trade::CAccountData data = mIter->second;
	data.m_bReady = data.m_bReady && m_bRunning.load() && m_pAccountService->IsConnected(accountIndex);
	return data;
}

void CTradeService::ApplyEvent(trade::CAccountData& data, trade::CCountEvent& event)
{
	if (trade::EventTy::Funds != event.m_kind && !event.m_bAccountValid)
	{
		bool bConnected = event.m_account.m_bConnected;
		int state = event.m_account.m_nState;
		event.m_account = data.m_account;
		if (trade::EventTy::Session == event.m_kind)
		{
			event.m_account.m_bConnected = bConnected;
			event.m_account.m_nState = state;
			if (!bConnected)
			{
				event.m_account.m_bFundsValid = false;
			}
		}
		event.m_bAccountValid = true;
	}
	if (trade::EventTy::Deal == event.m_kind)
	{
		for (const auto& order : data.m_orders)
		{
			if (event.m_trade.m_orderId == order.m_orderId)
			{
				event.m_order = order;
				break;
			}
		}
	}
	std::uint64_t previous = data.m_version;
	trade::UpdateAccountData(data, event);
	if (0 == event.m_sequence)
	{
		data.m_version = previous;
	}
}

void CTradeService::OnBrokerEvent(trade::CCountEvent event)
{
	if (!m_bRunning.load())
	{
		return;
	}
	if (trade::EventTy::Session == event.m_kind)
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_syncing.erase(event.m_accountIndex);
		trade::CAccountData& data = m_snapshots[event.m_accountIndex];
		data.m_bReady = false;
		data.m_account.m_bConnected = event.m_account.m_bConnected;
		data.m_account.m_bFundsValid = false;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		auto pending = m_syncing.find(event.m_accountIndex);
		if (m_syncing.end() != pending)
		{
			if (4096 <= pending->second.m_events.size())
			{
				pending->second.m_bOverflow = true;
			}
			else
			{
				pending->second.m_events.emplace_back(event);
			}
			return;
		}
		auto iter = m_snapshots.find(event.m_accountIndex);
		if (m_snapshots.end() == iter || !iter->second.m_bReady || (0 != event.m_sequence && event.m_sequence <= iter->second.m_version))
		{
			return;
		}
		ApplyEvent(iter->second, event);
	}
	PublishEvent(event);
}

bool CTradeService::IsReady(const _TyAccount& accountIndex) const
{
	return m_bRunning.load() && GetAccount(accountIndex).m_bReady;
}

void CTradeService::SetEventHandler(_TyEventHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_eventHandler = std::move(handler);
}

void CTradeService::SetPushHandler(_TyPushHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_pushHandler = std::move(handler);
}

void CTradeService::SetAccountReadyHandler(_TyAccountReadyHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_accountReadyHandler = std::move(handler);
}

void CTradeService::PublishEvent(const trade::CCountEvent& event)
{
	_TyEventHandler eventHandler;
	_TyPushHandler pushHandler;
	_TyAccountReadyHandler readyHandler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		if (0 != event.m_sequence && event.m_sequence <= m_lastEventSequence[event.m_accountIndex])
		{
			return;
		}
		if (0 != event.m_sequence)
		{
			m_lastEventSequence[event.m_accountIndex] = event.m_sequence;
		}
		eventHandler = m_eventHandler;
		pushHandler = m_pushHandler;
		readyHandler = m_accountReadyHandler;
	}
	if (nullptr != eventHandler)
	{
		eventHandler(event);
	}
	if (trade::EventTy::Session == event.m_kind)
	{
		if (IsReady(event.m_accountIndex) && nullptr != readyHandler)
		{
			readyHandler(event.m_accountIndex);
		}
	}
	else if (nullptr != pushHandler)
	{
		pushHandler(trade::protocol::Push(event, {}));
	}
}

void CTradeService::OnBrokerState(SessionState state)
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
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_lastEventSequence.clear();
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
	if (!m_pBroker->IsReady())
	{
		return;
	}
	for (const auto& account : accounts)
	{
		QueryAccount(account);
	}
}

void CTradeService::OnAccountState(const _TyAccount& accountIndex)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_syncing.erase(accountIndex);
		const auto mIter = m_snapshots.find(accountIndex);
		if (m_snapshots.end() != mIter)
		{
			mIter->second.m_bReady = false;
			mIter->second.m_account.m_bFundsValid = false;
			if (!m_pAccountService->HasAccount(accountIndex))
			{
				m_snapshots.erase(mIter);
			}
		}
	}
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_lastEventSequence.erase(accountIndex);
}
