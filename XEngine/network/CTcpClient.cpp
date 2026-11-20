#include "CTcpClient.h"
#include "CNetTools.h"
#include "../basic/CDistributor.h"
#include "../request/request.h"
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <utility>
#include <vector>
#include "CNetPool.h"

namespace net
{
	CTcpClient::CTcpClient(const std::string& strAddr, int nPort, bool bOrdered) : m_addr(strAddr), m_port(nPort)
	{
		if (bOrdered)
		{
			m_ordered_dispatcher = std::make_unique<_TyOrderedDistributor>();
		}
		else
		{
			m_dispatcher = std::make_unique<_TyDistributor>();
		}
	}

	CTcpClient::~CTcpClient()
	{
		Release();
	}

	void CTcpClient::CBufferEventDeleter::operator()(bufferevent* pEvent) const
	{
		if (nullptr != pEvent)
		{
			bufferevent_free(pEvent);
		}
	}

	int CTcpClient::Initialize()
	{
		if ((nullptr == GetNet()) || m_addr.empty() || (0 >= m_port) || (65535 < m_port))
		{
			return -1;
		}

		int nOptions = BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS;
		if (net::IsThreadEnable())
		{
			nOptions |= BEV_OPT_THREADSAFE;
		}
		if (nullptr != m_event)
		{
			return 0;
		}

		decltype(m_event) pEvent;
		pEvent.reset(bufferevent_socket_new(GetNet(), -1, nOptions));
		if (nullptr == pEvent)
		{
			return -2;
		}

		sockaddr_in svr{};
		if (!net::FmtAddress(svr, m_port, m_addr))
		{
			return -3;
		}
		bufferevent_setcb(pEvent.get(), CTcpClient::Read_Callback, nullptr, CTcpClient::Event_Callback, this);
		if (0 != bufferevent_enable(pEvent.get(), EV_READ | EV_WRITE))
		{
			return -4;
		}
		if (0 != bufferevent_socket_connect(pEvent.get(), reinterpret_cast<sockaddr*>(&svr), sizeof(svr)))
		{
			return -5;
		}
		m_event = std::move(pEvent);
		return 0;
	}

	void CTcpClient::Release()
	{
		ShutDown();
		if (m_connected && (0 <= m_router_id))
		{
			CNetPool::InstancePtr()->CloseAConnection(m_router_id);
		}
		m_event.reset();
		m_connected = false;
		m_router_id = -1;
		if (nullptr != m_dispatcher)
		{
			m_dispatcher->StopAndWait();
		}
	}

	net::_TyConnectionId CTcpClient::GetId() const
	{
		return m_router_id;
	}

	bool CTcpClient::Send(const void* pData, std::size_t nLength)
	{
		if ((nullptr == pData) || (0 == nLength) || !m_connected)
		{
			return false;
		}
		return (nullptr != m_event) && (0 == bufferevent_write(m_event.get(), pData, nLength));
	}

	bool CTcpClient::SendRequest(const CRequest& req)
	{
		if (!m_connected || (0 > m_router_id))
		{
			return false;
		}

		return net::SendRequest(GetId(), req);
	}

	bool CTcpClient::SetReadTimeout(int nSeconds)
	{
		return m_connected && (0 <= m_router_id) && CNetPool::InstancePtr()->SetReadTimeout(m_router_id, nSeconds);
	}

	void CTcpClient::RegisterHandler(_TyHandler&& handler)
	{
		if (nullptr != m_ordered_dispatcher)
		{
			m_ordered_dispatcher->RegisterHandler(std::move(handler));
			return;
		}
		if (nullptr != m_dispatcher)
		{
			m_dispatcher->RegisterHandler(std::move(handler));
		}
	}

	void CTcpClient::ClearHandlers()
	{
		if (nullptr != m_ordered_dispatcher)
		{
			m_ordered_dispatcher->ClearHandlers();
		}
		if (nullptr != m_dispatcher)
		{
			m_dispatcher->ClearHandlers();
		}
	}

	bool CTcpClient::IsConnected() const
	{
		return m_connected;
	}

	std::size_t CTcpClient::OnRead(bufferevent* pEvent)
	{
		std::vector<std::unique_ptr<CRequest>> reqs;
		std::size_t sz = net::RequestFromBuffer(reqs, pEvent);
		if (nullptr != m_dispatcher || nullptr != m_ordered_dispatcher)
		{
			std::vector<CNetEvent> events;
			events.reserve(reqs.size());
			for (auto& v : reqs)
			{
				events.emplace_back(em_event::request);
				auto& ev = events.back();
				ev.m_request = std::move(v);
			}
			Dispatch(std::move(events));
		}
		return sz;
	}

	void CTcpClient::OnConnected(bufferevent* pEvent)
	{
		m_connected = true;
		m_router_id = CNetPool::InstancePtr()->RegisterAConnection(pEvent);
		if (0 <= m_router_id)
		{
			m_event.release();
		}
		if (nullptr != m_dispatcher || nullptr != m_ordered_dispatcher)
		{
			std::vector<CNetEvent> events;
			events.emplace_back(em_event::connected);
			Dispatch(std::move(events));
		}
	}

	void CTcpClient::OnEvent(bufferevent* pEvent, short events)
	{
		if (nullptr == pEvent)
		{
			return;
		}
		if (0 == (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)))
		{
			return;
		}

		_TyConnectionId id = CNetPool::InstancePtr()->CloseAConnection(pEvent);
		m_connected = false;
		m_router_id = -1;
		if ((id >= 0) && (nullptr != m_dispatcher || nullptr != m_ordered_dispatcher))
		{
			std::vector<CNetEvent> events;
			events.emplace_back(em_event::disconnected, id);
			Dispatch(std::move(events));
		}
		ShutDown();
	}
} // namespace net

void net::CTcpClient::Dispatch(std::vector<CNetEvent>&& events)
{
	// Ordered sessions consume stream messages on their network thread before the next frame.
	if (nullptr != m_ordered_dispatcher)
	{
		m_ordered_dispatcher->Dispatch(std::move(events));
	}
	else if (nullptr != m_dispatcher)
	{
		m_dispatcher->Dispatch(std::move(events));
	}
}
