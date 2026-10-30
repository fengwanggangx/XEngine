#include "CBrokerClient.h"
#include "TradeProtocol.h"
#include "../request/request.pb.h"
#include <algorithm>
#include <unordered_set>
namespace
{
	CLoginInfo OrderedInfo(const CLoginInfo& info)
	{
		CLoginInfo ordered = info;
		ordered.m_bOrdered = true;
		return ordered;
	}
} // namespace
CBrokerClient::CBrokerClient(const CLoginInfo& info) : m_session(OrderedInfo(info))
{
	m_session.RegisterHandler([this](const CRequest& response)
	{
		OnResponse(response);
	});
	m_session.RegisterStateHandler([this](SessionState state, const std::string&)
	{
		OnState(state);
	});
}
CBrokerClient::~CBrokerClient()
{
	Stop();
}
bool CBrokerClient::Start()
{
	if (!m_session.Start())
	{
		return false;
	}
	m_refresh = std::jthread([this](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
			Refresh();
			std::unique_lock<std::mutex> lock(m_mtx_wait);
			m_wake.wait_for(lock, std::chrono::seconds(1), [&stop]()
			{
				return stop.stop_requested();
			});
		}
	});
	return true;
}
void CBrokerClient::Stop()
{
	m_refresh.request_stop();
	m_wake.notify_all();
	if (m_refresh.joinable())
	{
		m_refresh.join();
	}
	m_session.Stop();
	FailPending();
}
void CBrokerClient::SetCatalogueHandler(WireHandler handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	m_catalogueHandler = std::move(handler);
}
void CBrokerClient::SetEventHandler(EventHandler handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	m_eventHandler = std::move(handler);
}
bool CBrokerClient::Forward(CRequest request, WireHandler handler)
{
	if (!m_session.IsAuthenticated())
	{
		return false;
	}
	std::uint64_t nId = m_nNextId.fetch_add(1);
	request.SetExtraData("source_request_id", std::to_string(request.GetId()));
	request.SetId(nId);
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (4096 <= m_pending.size())
		{
			return false;
		}
		m_pending.emplace(nId, CPending{ std::move(handler), std::chrono::steady_clock::now() + std::chrono::seconds(_TyRequest::TRADE_ACCOUNT_LOGIN == request.GetType() ? 60 : 15), request.GetType() });
	}
	if (!m_session.SendRequest(request))
	{
		// A write failure may follow a partial write. Complete as uncertain, never retry.
		CRequest response;
		response.SetId(nId);
		response.SetType(request.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker write failed; reconcile before retry");
		OnResponse(response);
	}
	return true;
}
trade::CResult CBrokerClient::Request(trade::CReqParam request, ReplyHandler handler)
{
	if (!IsAccountValid(request.m_accountIndex))
	{
		return { trade::Error::NotFound, "broker_id/account required" };
	}
	_TyAccount accountIndex = request.m_accountIndex;
	std::uint64_t nRequestId = request.m_requestId;
	trade::Operation operation = request.GetOperation();
	std::uint64_t nGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		nGeneration = m_generation;
	}
	CRequest wire = trade::protocol::WriteRequest(request);
	bool bQueued = Forward(std::move(wire), [this, accountIndex, nRequestId, operation, nGeneration, handler = std::move(handler)](const CRequest& response)
	{
		CompleteRequest(response, accountIndex, nRequestId, operation, nGeneration, handler);
	});
	return bQueued ? trade::CResult{} : trade::CResult{ trade::Error::Unavailable, "XBroker is unavailable" };
}
void CBrokerClient::CompleteRequest(const CRequest& response, const _TyAccount& accountIndex, std::uint64_t nRequestId, trade::Operation operation, std::uint64_t nGeneration, const ReplyHandler& handler)
{
	trade::CCounterReply reply;
	if (!trade::protocol::ReadReply(response, reply) || (reply.m_result.Ok() && (accountIndex != reply.m_accountIndex || operation != reply.m_operation)))
	{
		reply.m_result = { trade::Error::Unavailable, "Invalid XBroker response" };
		reply.m_data = std::monostate{};
	}
	reply.m_requestId = nRequestId;
	reply.m_accountIndex = accountIndex;
	reply.m_operation = operation;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (nGeneration != m_generation)
		{
			reply.m_result = { trade::Error::UnknownResult, "Stale XBroker connection" };
			reply.m_data = std::monostate{};
		}
	}
	if (nullptr != handler)
	{
		handler(reply);
	}
}

void CBrokerClient::QueryAccount(const _TyAccount& accountIndex)
{
	std::uint64_t nGeneration = 0;
	std::uint64_t nCycle = m_nNextId.fetch_add(1);
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

void CBrokerClient::QueryNext(const _TyAccount& accountIndex, std::uint64_t nGeneration, std::uint64_t nCycle, trade::Operation operation)
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
	trade::CResult result = Request(std::move(request), [this, nGeneration, nCycle](const trade::CCounterReply& reply)
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

void CBrokerClient::CompleteQuery(const trade::CCounterReply& reply, std::uint64_t nGeneration, std::uint64_t nCycle)
{
	_TyAccount accountIndex = reply.m_accountIndex;
	EventHandler eventHandler;
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
		if (!reply.m_result.Ok() || pending->second.m_bOverflow)
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
			cached.m_bReady = cached.m_account.m_bConnected && cached.m_account.m_bFundsValid;
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
			eventHandler = m_eventHandler;
			m_syncing.erase(pending);
		}
	}
	if (bNext)
	{
		QueryNext(accountIndex, nGeneration, nCycle, next);
	}
	if (nullptr != eventHandler)
	{
		for (const auto& event : notifications)
		{
			eventHandler(event);
		}
	}
}

trade::CAccountData CBrokerClient::Snapshot(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	auto iter = m_snapshots.find(accountIndex);
	if (!IsAccountValid(accountIndex) || m_snapshots.end() == iter)
	{
		trade::CAccountData data;
		data.m_account.m_accountIndex = accountIndex;
		return data;
	}
	return iter->second;
}
void CBrokerClient::ApplyEvent(trade::CAccountData& data, trade::CCountEvent& event)
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
void CBrokerClient::OnResponse(const CRequest& response)
{
	WireHandler handler;
	EventHandler eventHandler;
	if (0 == response.GetId())
	{
		trade::CCountEvent event;
		if (!trade::protocol::ReadPush(response, event))
		{
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
			if (m_snapshots.end() == iter || (!iter->second.m_bReady && trade::EventTy::Session != event.m_kind) || (0 != event.m_sequence && event.m_sequence <= iter->second.m_version))
			{
				return;
			}
			ApplyEvent(iter->second, event);
			if (trade::EventTy::Session == event.m_kind)
			{
				iter->second.m_bReady = false;
				iter->second.m_account.m_bFundsValid = false;
			}
			eventHandler = m_eventHandler;
		}
		if (nullptr != eventHandler)
		{
			eventHandler(event);
		}
		return;
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		auto iter = m_pending.find(response.GetId());
		if (m_pending.end() == iter)
		{
			return;
		}
		handler = std::move(iter->second.m_handler);
		m_pending.erase(iter);
	}
	if (nullptr != handler)
	{
		handler(response);
	}
}
void CBrokerClient::OnState(SessionState state)
{
	if (SessionState::Ready == state)
	{
		return;
	}
	EventHandler handler;
	std::vector<trade::CCountEvent> events;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		handler = m_eventHandler;
		++m_generation;
		m_syncing.clear();
		for (auto& item : m_snapshots)
		{
			item.second.m_bReady = false;
			item.second.m_account.m_bConnected = false;
			trade::CCountEvent event;
			event.m_accountIndex = item.first;
			event.m_account = item.second.m_account;
			events.emplace_back(event);
		}
	}
	if (nullptr != handler)
	{
		for (const auto& event : events)
		{
			handler(event);
		}
	}
	FailPending();
}
void CBrokerClient::FailPending()
{
	std::unordered_map<std::uint64_t, CPending> pending;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		pending.swap(m_pending);
	}
	for (const auto& item : pending)
	{
		CRequest response;
		response.SetId(item.first);
		response.SetType(item.second.m_type);
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker disconnected; reconcile before retry");
		if (nullptr != item.second.m_handler)
		{
			item.second.m_handler(response);
		}
	}
}
void CBrokerClient::Refresh()
{
	std::vector<std::pair<std::uint64_t, _TyRequest>> expired;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		for (const auto& item : m_pending)
		{
			if (item.second.m_deadline <= std::chrono::steady_clock::now())
			{
				expired.emplace_back(item.first, item.second.m_type);
			}
		}
	}
	for (const auto& item : expired)
	{
		CRequest response;
		response.SetId(item.first);
		response.SetType(item.second);
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker request timed out; reconcile before retry");
		OnResponse(response);
	}
	if (!m_session.IsAuthenticated())
	{
		return;
	}
	CRequest query;
	query.SetType(_TyRequest::TRADE_ACCOUNT_LIST_QUERY);
	Forward(std::move(query), [this](const CRequest& response)
	{
		if (response.GetErrorInfo().has_value() || !response.GetData().has_account_list())
		{
			return;
		}
		WireHandler handler;
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			handler = m_catalogueHandler;
		}
		if (nullptr != handler)
		{
			handler(response);
		}
		for (const auto& account : response.GetData().account_list().accounts())
		{
			QueryAccount(AccountIndex(account));
		}
	});
}
