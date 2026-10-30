#include "CTcpServer.h"
#include <event2/buffer.h> // 主要头文件
#include "CNetTools.h"
#include <iostream>
#include <event2/thread.h>
#include <event2/listener.h>
#include <chrono>
#include <thread>
#include "common_net.h"
#include "CNetPool.h"
#include "../request/request.h"
#include "../basic/CDistributor.h"

namespace net
{
	CTcpServer::~CTcpServer() = default;

	CTcpServer::CTcpServer(int nPort) : m_nPort(nPort), m_dispatcher(std::make_unique<_TyDistributor>())
	{
	}

	void CTcpServer::Drain()
	{
		m_dispatcher->StopAndWait();
	}

	void CTcpServer::ClearHandlers()
	{
		m_dispatcher->ClearHandlers();
	}

	void CTcpServer::OnListenerError(struct evconnlistener* pListener)
	{
	}

	void CTcpServer::OnConnAccept(struct evconnlistener* pListener, _TyConnectionId id, struct sockaddr* pAddr, int nLength)
	{
		if (nullptr == GetNet())
		{
			return;
		}

		struct bufferevent* pBuffer = CNetPool::InstancePtr()->RegisterConnect(id, GetNet(), pAddr, nLength, CTcpServer::Read_Callback, nullptr, CTcpServer::Event_Callback, this);
		if (nullptr != pBuffer)
		{
#if defined(XENGINE_DEBUG_BUILD)
			timeval readTimeout{ 7200, 0 };
#else
			timeval readTimeout{ 20, 0 };
#endif
			bufferevent_set_timeouts(pBuffer, &readTimeout, nullptr);
		}
	}

	std::size_t CTcpServer::OnRead(struct bufferevent* pEvent)
	{
		std::vector<std::unique_ptr<CRequest>> reqs;
		std::size_t sz = net::RequestFromBuffer(reqs, pEvent);
		if (m_dispatcher != nullptr)
		{
			std::vector<CNetEvent> events;
			events.reserve(reqs.size());
			for (auto& v : reqs)
			{
				events.emplace_back(em_event::request);
				auto& ev = events.back();
				ev.m_connection_id = v->GetConnectionId();
				ev.m_request = std::move(v);
			}
			m_dispatcher->Dispatch(std::move(events));
		}
		return sz;
	}

	void CTcpServer::OnEvent(struct bufferevent* pEvent, short events)
	{
		if (pEvent == nullptr)
		{
			return;
		}
		if (0 == (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR | BEV_EVENT_TIMEOUT)))
		{
			return;
		}

		_TyConnectionId id = CNetPool::InstancePtr()->CloseAConnection(pEvent);
		if ((id >= 0) && (nullptr != m_dispatcher))
		{
			std::vector<CNetEvent> events;
			events.emplace_back(em_event::disconnected, id);
			m_dispatcher->Dispatch(std::move(events));
		}
	}

	int CTcpServer::Initialize()
	{
		if (nullptr == GetNet())
		{
			return -1;
		}
		struct sockaddr_in svr;
		if (!FmtAddress(svr, m_nPort))
		{
			return -1;
		}
		struct evconnlistener* pListener = evconnlistener_new_bind(
			GetNet(),
			CTcpServer::ConnAccept_Callback,
			this,
			LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
			-1,
			(struct sockaddr*)&svr,
			sizeof(svr));

		if (nullptr == pListener)
		{
			return -1;
		}
		// evconnlistener_set_error_cb(pListener, ListenerErrorCallback);
		return 0;
	}

	void CTcpServer::RegisterHandler(_TyHandler&& func)
	{
		if (nullptr != m_dispatcher)
		{
			m_dispatcher->RegisterHandler(std::move(func));
		}
	}
} // namespace net
