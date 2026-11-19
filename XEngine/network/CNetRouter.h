#ifndef __CNETROUTER_H__
#define __CNETROUTER_H__

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include "CNet.h"
#include "common_net.h"

struct bufferevent;

namespace net
{
	template <typename _Ty>
	class CNetRouter
	{
	public:
		CNetRouter() = default;
		virtual ~CNetRouter() = default;

	protected:
		static void ConnAccept_Callback(struct evconnlistener* pListener, _TyConnectionId id, struct sockaddr* pAddr, int nLength, void* pArg);
		static void Event_Callback(struct bufferevent* pEvent, short events, void* pArg);
		static void Read_Callback(struct bufferevent* pEvent, void* pArg);
		static void Write_Callback(struct bufferevent* pEvent, void* pArg);

	public:
		virtual void OnConnAccept(struct evconnlistener* pListener, _TyConnectionId id, struct sockaddr* pAddr, int nLength)
		{
		}

		virtual void OnConnected(struct bufferevent* pEvent)
		{
		}

		virtual void OnEvent(struct bufferevent* pEvent, short events)
		{
		}

		virtual std::size_t OnRead(struct bufferevent* pEvent)
		{
			return 0;
		}

		virtual void OnWrite(struct bufferevent* pEvent)
		{
		}
	};

	template <typename _Ty>
	void CNetRouter<_Ty>::ConnAccept_Callback(struct evconnlistener* pListener, _TyConnectionId id, struct sockaddr* pAddr, int nLength, void* pArg)
	{
		_Ty* pInstance = static_cast<_Ty*>(pArg);
		if (nullptr != pInstance)
		{
			pInstance->OnConnAccept(pListener, id, pAddr, nLength);
		}
	}

	template <typename _Ty>
	void CNetRouter<_Ty>::Event_Callback(struct bufferevent* pEvent, short events, void* pArg)
	{
		_Ty* pInstance = static_cast<_Ty*>(pArg);
		if (nullptr != pInstance)
		{
			if (0 != (events & BEV_EVENT_CONNECTED))
			{
				pInstance->OnConnected(pEvent);
			}
			else
			{
				pInstance->OnEvent(pEvent, events);
			}
		}
	}

	template <typename _Ty>
	void CNetRouter<_Ty>::Read_Callback(struct bufferevent* pEvent, void* pArg)
	{
		_Ty* pInstance = static_cast<_Ty*>(pArg);
		if (nullptr != pInstance)
		{
			pInstance->OnRead(pEvent);
		}
	}

	template <typename _Ty>
	void CNetRouter<_Ty>::Write_Callback(struct bufferevent* pEvent, void* pArg)
	{
		_Ty* pInstance = static_cast<_Ty*>(pArg);
		if (nullptr != pInstance)
		{
			pInstance->OnWrite(pEvent);
		}
	}
} // namespace net
#endif
