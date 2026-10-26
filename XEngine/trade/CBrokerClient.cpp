#include "CBrokerClient.h"
#include "TradeProtocol.h"
#include "../request/request.pb.h"
#include <algorithm>
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
	trade::Id nAccountId = request.m_accountId;
	trade::Id nRequestId = request.m_requestId;
	bool bFullSnapshot = trade::Operation::Synchronize == request.m_operation;
	CRequest wire = trade::protocol::WriteRequest(request);
	bool bQueued = Forward(std::move(wire), [this, nAccountId, nRequestId, bFullSnapshot, handler = std::move(handler)](const CRequest& response)
	{
		trade::CReply reply;
		if (!trade::protocol::ReadReply(response, reply))
		{
			reply.m_result = { trade::Error::Unavailable, "Invalid XBroker response" };
		}
		reply.m_requestId = nRequestId;
		EventHandler eventHandler;
		std::vector<trade::CEvent> sessions;
		if (reply.m_result.Ok() && bFullSnapshot)
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
			trade::CSnapshot& cached = m_snapshots[nAccountId];
			if (!cached.m_bReady || cached.m_version <= reply.m_snapshot.m_version)
			{
				bool bWasReady = cached.m_bReady;
				cached = reply.m_snapshot;
				if (!bWasReady && cached.m_bReady)
				{
					for (const auto& account : cached.m_accounts)
					{
						trade::CEvent event;
						event.m_accountId = account.m_accountId;
						event.m_kind = trade::EventKind::Session;
						event.m_account = account;
						sessions.push_back(event);
					}
				}
			}
			eventHandler = m_eventHandler;
		}
		if (nullptr != eventHandler)
		{
			// Reconcile orders/trades before allowing strategies to resume. Final session
			// state comes from the snapshot, never from a historical login event.
			for (auto& event : reply.m_events)
			{
				if (trade::EventKind::Trade == event.m_kind)
				{
					for (const auto& order : reply.m_snapshot.m_orders)
					{
						if (event.m_trade.m_orderId == order.m_orderId)
						{
							event.m_order = order;
							break;
						}
					}
				}
				if (trade::EventKind::Session != event.m_kind)
				{
					eventHandler(event);
				}
			}
			for (const auto& event : sessions)
			{
				eventHandler(event);
			}
		}
		if (nullptr != handler)
		{
			handler(reply);
		}
	});
	return bQueued ? trade::CResult{} : trade::CResult{ trade::Error::Unavailable, "XBroker is unavailable" };
}
trade::CSnapshot CBrokerClient::Snapshot(trade::Id nAccountId, bool bHistory) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	auto iter = m_snapshots.find(nAccountId);
	trade::CSnapshot result;
	if (m_snapshots.end() != iter)
	{
		result.m_bReady = iter->second.m_bReady;
		result.m_version = iter->second.m_version;
		result.m_nTime = iter->second.m_nTime;
		result.m_accounts = iter->second.m_accounts;
		result.m_positions = iter->second.m_positions;
		if (bHistory)
		{
			result.m_orders = iter->second.m_orders;
			result.m_trades = iter->second.m_trades;
		}
	}
	return result;
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
			auto iter = m_snapshots.find(event.m_accountId);
			if (m_snapshots.end() == iter || !iter->second.m_bReady || event.m_sequence <= iter->second.m_version)
			{
				return;
			}
			if (trade::EventKind::Funds != event.m_kind)
			{
				bool bConnected = event.m_account.m_bConnected;
				for (const auto& account : iter->second.m_accounts)
				{
					if (event.m_accountId == account.m_accountId)
					{
						event.m_account = account;
						break;
					}
				}
				if (trade::EventKind::Session == event.m_kind)
				{
					event.m_account.m_bConnected = bConnected;
				}
			}
			if (trade::EventKind::Trade == event.m_kind)
			{
				for (const auto& order : iter->second.m_orders)
				{
					if (event.m_trade.m_orderId == order.m_orderId)
					{
						event.m_order = order;
						break;
					}
				}
			}
			trade::UpdateSnapshot(iter->second, event);
			iter->second.m_version = event.m_sequence;
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
		for (auto& item : m_snapshots)
		{
			item.second.m_bReady = false;
			for (auto& account : item.second.m_accounts)
			{
				account.m_bConnected = false;
				trade::CEvent event;
				event.m_accountId = account.m_accountId;
				event.m_account = account;
				events.push_back(event);
			}
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
			sync.m_operation = trade::Operation::Synchronize;
			sync.m_afterSequence = Snapshot(account.account_id(), false).m_version;
			Request(std::move(sync), {});
		}
	});
}
