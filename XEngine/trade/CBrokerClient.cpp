#include "CBrokerClient.h"
#include "TradeProtocol.h"
#include "../request/request.pb.h"
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

void CBrokerClient::SetStateHandler(_TyStateHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	m_stateHandler = std::move(handler);
}

void CBrokerClient::SetEventHandler(_TyEventHandler handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	m_eventHandler = std::move(handler);
}

bool CBrokerClient::Forward(CRequest request, _TyWireHandler handler)
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

trade::CResult CBrokerClient::SendRequest(trade::CReqParam request, _TyReplyHandler handler)
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

void CBrokerClient::CompleteRequest(const CRequest& response, const _TyAccount& accountIndex, std::uint64_t nRequestId, trade::Operation operation, std::uint64_t nGeneration, const _TyReplyHandler& handler)
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

void CBrokerClient::OnResponse(const CRequest& response)
{
	_TyWireHandler handler;
	_TyEventHandler eventHandler;
	if (0 == response.GetId())
	{
		trade::CCountEvent event;
		if (!trade::protocol::ReadPush(response, event))
		{
			return;
		}
		{
			std::lock_guard<std::mutex> lock(m_mtx_state);
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
	_TyStateHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (SessionState::Ready != state)
		{
			++m_generation;
		}
		handler = m_stateHandler;
	}
	if (nullptr != handler)
	{
		handler(state);
	}
	if (SessionState::Ready != state)
	{
		FailPending();
	}
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
}

bool CBrokerClient::IsReady() const
{
	return m_session.IsAuthenticated();
}
