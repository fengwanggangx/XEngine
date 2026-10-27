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
		m_pending.emplace(nId, CPending{ std::move(handler), std::chrono::steady_clock::now() + std::chrono::seconds(10), request.GetType() });
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
trade::CResult CBrokerClient::Request(trade::CRequest request, ReplyHandler handler)
{
	if (0 == request.m_accountId)
	{
		return { trade::Error::NotFound, "account_id must be positive" };
	}
	std::uint64_t nAccountId = request.m_accountId;
	std::uint64_t nRequestId = request.m_requestId;
	trade::Operation operation = request.GetOperation();
	bool bSynchronize = trade::Operation::Synchronize == operation;
	std::uint64_t nGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		nGeneration = m_generation;
		if (bSynchronize && !m_syncing.try_emplace(nAccountId).second)
		{
			return { trade::Error::Conflict, "account synchronization in progress" };
		}
	}
	CRequest wire = trade::protocol::WriteRequest(request);
	bool bQueued = Forward(std::move(wire), [this, nAccountId, nRequestId, operation, bSynchronize, nGeneration, handler = std::move(handler)](const CRequest& response)
	{
		CompleteRequest(response, nAccountId, nRequestId, operation, nGeneration, handler);
	});
	if (!bQueued && bSynchronize)
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (nGeneration == m_generation)
		{
			m_syncing.erase(nAccountId);
		}
	}
	return bQueued ? trade::CResult{} : trade::CResult{ trade::Error::Unavailable, "XBroker is unavailable" };
}
void CBrokerClient::CompleteRequest(const CRequest& response, std::uint64_t nAccountId, std::uint64_t nRequestId, trade::Operation operation, std::uint64_t nGeneration, const ReplyHandler& handler)
{
	bool bSynchronize = trade::Operation::Synchronize == operation;
	trade::CCounterReply reply;
	if (!trade::protocol::ReadReply(response, reply) || (reply.m_result.Ok() && (nAccountId != reply.m_accountId || operation != reply.m_operation)))
	{
		reply.m_result = { trade::Error::Unavailable, "Invalid XBroker response" };
		reply.m_data = std::monostate{};
	}
	reply.m_requestId = nRequestId;
	reply.m_accountId = nAccountId;
	reply.m_operation = operation;
	EventHandler eventHandler;
	std::vector<trade::CEvent> notifications;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (nGeneration != m_generation)
		{
			reply.m_result = { trade::Error::UnknownResult, "Stale XBroker connection" };
			reply.m_data = std::monostate{};
		}
		else if (bSynchronize)
		{
			auto pending = m_syncing.find(nAccountId);
			const trade::CAccountData* source = std::get_if<trade::CAccountData>(&reply.m_data);
			if (reply.m_result.Ok() && nullptr != source && source->m_bReady && m_syncing.end() != pending && !pending->second.m_bOverflow)
			{
				trade::CAccountData& cached = m_snapshots[nAccountId];
				std::unordered_set<std::uint64_t> previousTrades;
				previousTrades.reserve(cached.m_trades.size());
				for (const auto& row : cached.m_trades)
				{
					previousTrades.emplace(row.m_tradeId);
				}
				cached = *source;
				// Reconcile complete state before the final session-ready notification.
				for (const auto& order : cached.m_orders)
				{
					trade::CEvent event;
					event.m_accountId = nAccountId;
					event.m_kind = trade::EventKind::Order;
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
					trade::CEvent event;
					event.m_accountId = nAccountId;
					event.m_kind = trade::EventKind::Trade;
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
				std::stable_sort(pending->second.m_events.begin(), pending->second.m_events.end(), [](const trade::CEvent& lhs, const trade::CEvent& rhs)
				{
					return lhs.m_sequence < rhs.m_sequence;
				});
				for (auto& event : pending->second.m_events)
				{
					if (cached.m_version < event.m_sequence)
					{
						ApplyEvent(cached, event);
						notifications.emplace_back(event);
					}
				}
				trade::CEvent session;
				session.m_accountId = nAccountId;
				session.m_kind = trade::EventKind::Session;
				session.m_account = cached.m_account;
				notifications.emplace_back(session);
				eventHandler = m_eventHandler;
			}
			else
			{
				m_snapshots[nAccountId].m_bReady = false;
				if (reply.m_result.Ok())
				{
					reply.m_result = { trade::Error::Unavailable, "Incomplete account synchronization" };
					reply.m_data = std::monostate{};
				}
			}
			if (m_syncing.end() != pending)
			{
				m_syncing.erase(pending);
			}
		}
	}
	if (nullptr != eventHandler)
	{
		for (const auto& event : notifications)
		{
			eventHandler(event);
		}
	}
	if (nullptr != handler)
	{
		handler(reply);
	}
}
trade::CAccountData CBrokerClient::Snapshot(std::uint64_t nAccountId) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	auto iter = m_snapshots.find(nAccountId);
	if (0 == nAccountId || m_snapshots.end() == iter)
	{
		trade::CAccountData data;
		data.m_account.m_accountId = nAccountId;
		return data;
	}
	return iter->second;
}
void CBrokerClient::ApplyEvent(trade::CAccountData& data, trade::CEvent& event)
{
	if (trade::EventKind::Funds != event.m_kind)
	{
		bool bConnected = event.m_account.m_bConnected;
		event.m_account = data.m_account;
		if (trade::EventKind::Session == event.m_kind)
		{
			event.m_account.m_bConnected = bConnected;
		}
	}
	if (trade::EventKind::Trade == event.m_kind)
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
	trade::UpdateAccountData(data, event);
}
std::vector<trade::Operation> CBrokerClient::Capabilities() const
{
	return {}; // Per-account capabilities are queried from XBroker, never inferred here.
}
void CBrokerClient::OnResponse(const CRequest& response)
{
	WireHandler handler;
	EventHandler eventHandler;
	if (0 == response.GetId())
	{
		trade::CEvent event;
		if (!trade::protocol::ReadPush(response, event))
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			auto pending = m_syncing.find(event.m_accountId);
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
			auto iter = m_snapshots.find(event.m_accountId);
			if (m_snapshots.end() == iter || !iter->second.m_bReady || event.m_sequence <= iter->second.m_version)
			{
				return;
			}
			ApplyEvent(iter->second, event);
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
	std::vector<trade::CEvent> events;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		handler = m_eventHandler;
		++m_generation;
		m_syncing.clear();
		for (auto& item : m_snapshots)
		{
			item.second.m_bReady = false;
			item.second.m_account.m_bConnected = false;
			trade::CEvent event;
			event.m_accountId = item.first;
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
	query.SetType(_TyRequest::TRADE_ACCOUNT_QUERY);
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
			trade::CRequest sync;
			sync.m_accountId = account.account_id();
			sync.m_params = trade::CSynchronize{};
			if (Snapshot(account.account_id()).m_bReady)
			{
				continue;
			}
			Request(std::move(sync), {});
		}
	});
}
