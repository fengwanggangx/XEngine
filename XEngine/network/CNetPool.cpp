#include <string.h>
#include <utility>
#include <vector>
#include <iostream>
#include "common_net.h"
#include "event2/event.h"
#include <event2/buffer.h>
#include "CFrameBuffer.h"
#include "CNetPool.h"
#include "../request/request.h"

namespace net
{
	struct CNetInfo
	{
		_TyConnectionId m_fd{ -1 };

		int m_nPort{ -1 };
		std::string m_strAddress;
		struct sockaddr_storage m_addr{ 0 };
		struct bufferevent* m_pEvent{ nullptr };

		CFrameBuffer m_frames;

		void Empty()
		{
			m_fd = -1;
			if (nullptr != m_pEvent)
			{
				bufferevent_free(m_pEvent);
				m_pEvent = nullptr;
			}

			m_nPort = -1;
			m_strAddress.clear();
			memset(&m_addr, 0, sizeof(m_addr));

			m_frames.Reset();
		}

		~CNetInfo()
		{
			Empty();
		}
	};

	CNetPool::CNetPool()
	{
	}

	CNetPool::~CNetPool()
	{
	}

	bool CNetPool::RegisterAConnection(_TyConnectionId id, struct bufferevent* pEvent, struct sockaddr* pAddr)
	{
		if ((nullptr == pAddr) || (nullptr == pEvent))
		{
			return false;
		}

		std::unique_lock<std::shared_mutex> lock(m_mtx_pool);
		auto [mIter, bInserted] = m_pool.try_emplace(id, nullptr);
		CNetInfo* pInfo = mIter->second.get();
		if (bInserted)
		{
			mIter->second = std::make_unique<CNetInfo>();
			pInfo = mIter->second.get();
		}
		pInfo->Empty();
		pInfo->m_fd = id;
		pInfo->m_pEvent = pEvent;
		SockAddrSafeCopy(pInfo->m_addr, *pAddr);
		ParseSockAddr(pInfo->m_strAddress, pInfo->m_nPort, *pAddr);
		return true;
	}

	_TyConnectionId CNetPool::RegisterAConnection(struct bufferevent* pEvent)
	{
		if (nullptr == pEvent)
		{
			return -1;
		}
		_TyConnectionId id = bufferevent_getfd(pEvent);

		sockaddr_storage addr{};
		ev_socklen_t nLen = sizeof(addr);
		if (0 != getpeername(id, reinterpret_cast<sockaddr*>(&addr), &nLen))
		{
			return -1;
		}

		RegisterAConnection(id, pEvent, &addr);
		return id;
	}

	struct bufferevent* CNetPool::RegisterAConnection(_TyConnectionId id, struct bufferevent* pEvent, struct sockaddr_storage* pAddr)
	{
		if ((nullptr == pAddr) || (nullptr == pEvent))
		{
			return nullptr;
		}

		std::unique_lock<std::shared_mutex> lock(m_mtx_pool);
		auto [mIter, bInserted] = m_pool.try_emplace(id, nullptr);
		CNetInfo* pInfo = mIter->second.get();
		if (bInserted)
		{
			mIter->second = std::make_unique<CNetInfo>();
			pInfo = mIter->second.get();
		}
		pInfo->Empty();
		pInfo->m_fd = id;
		pInfo->m_pEvent = pEvent;
		memset(&pInfo->m_addr, 0, sizeof(pInfo->m_addr)); // 初始化目标结构体（可选但安全）
		memcpy(&pInfo->m_addr, pAddr, sizeof(*pAddr));
		ParseSockAddr(pInfo->m_strAddress, pInfo->m_nPort, *pAddr);
		return pEvent;
	}

	struct bufferevent* CNetPool::RegisterConnect(_TyConnectionId id, struct event_base* pNet, struct sockaddr* pAddr, int nLength, bufferevent_data_cb readcb, bufferevent_data_cb writecb, bufferevent_event_cb eventcb, void* cbarg)
	{
		if (!CheckSockAddress(pAddr, nLength))
		{
			evutil_closesocket(id);
			return nullptr;
		}

		int nOptions = (BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS | BEV_OPT_UNLOCK_CALLBACKS);
		if (net::IsThreadEnable())
		{
			nOptions |= BEV_OPT_THREADSAFE;
		}

		struct bufferevent* pBuffer = bufferevent_socket_new(pNet, id, nOptions);
		if (nullptr == pBuffer)
		{
			std::cerr << "Failed to create bufferevent" << std::endl;
			evutil_closesocket(id);
			return nullptr;
		}

		bufferevent_setcb(pBuffer, readcb, writecb, eventcb, cbarg);
		bufferevent_enable(pBuffer, EV_READ | EV_WRITE);
		if (!RegisterAConnection(id, pBuffer, pAddr))
		{
			bufferevent_free(pBuffer);
			return nullptr;
		}
		return pBuffer;
	}

	bool CNetPool::CloseAConnection(CNetInfo& info)
	{
		struct bufferevent* pEvent = info.m_pEvent;
		if (nullptr == pEvent)
		{
			return false;
		}

		bufferevent_disable(pEvent, EV_READ | EV_WRITE);

		// 清空缓冲区
		evbuffer_drain(bufferevent_get_input(pEvent), -1);
		evbuffer_drain(bufferevent_get_output(pEvent), -1);

		bufferevent_free(pEvent);
		info.m_pEvent = nullptr;
		return true;
	}

	net::_TyConnectionId CNetPool::CloseAConnection(_TyConnectionId id)
	{
		std::unique_ptr<CNetInfo> pInfo;
		{
			std::unique_lock<std::shared_mutex> lock(m_mtx_pool);
			auto mIter = m_pool.find(id);
			if (mIter == m_pool.end())
			{
				return -1;
			}
			pInfo = std::move(mIter->second);
			m_pool.erase(mIter);
		}

		CloseAConnection(*pInfo);
		return id;
	}

	net::_TyConnectionId CNetPool::CloseAConnection(struct bufferevent* pEvent)
	{
		if (nullptr == pEvent)
		{
			return -1;
		}

		std::unique_ptr<CNetInfo> pInfo;
		_TyConnectionId id = -1;
		{
			std::unique_lock<std::shared_mutex> lock(m_mtx_pool);
			auto mIter = m_pool.begin();
			for (; mIter != m_pool.end(); ++mIter)
			{
				if (mIter->second->m_pEvent == pEvent)
				{
					break;
				}
			}
			if (mIter == m_pool.end())
			{
				return -1;
			}
			id = mIter->first;
			pInfo = std::move(mIter->second);
			m_pool.erase(mIter);
		}

		CloseAConnection(*pInfo);
		return id;
	}

	std::pair<RecvFrameResult, std::vector<std::string>> CNetPool::GetRecvFrames(_TyConnectionId id, struct bufferevent* pEvent, const char* data, std::size_t nLength)
	{
		std::pair<RecvFrameResult, std::vector<std::string>> ret = { RecvFrameResult::ConnectionNotFound, {} };

		if ((nullptr == pEvent) || ((nullptr == data) && (0 != nLength)))
		{
			return ret;
		}

		std::optional<std::vector<std::string>> frames{ std::nullopt };

		{
			std::unique_lock<std::shared_mutex> lock(m_mtx_pool);
			const auto mIter = m_pool.find(id);
			if ((mIter == m_pool.end()) || (mIter->second->m_pEvent != pEvent))
			{
				return ret;
			}
			frames = mIter->second->m_frames.Decode(data, nLength);
		}
		if (!frames.has_value())
		{
			ret.first = RecvFrameResult::ProtocolError;
			return ret;
		}

		return { RecvFrameResult::Ok, std::move(*frames) };
	}

	bool CNetPool::Send(_TyConnectionId id, const char* data, size_t nLength)
	{
		if ((nullptr == data) || (0 == nLength))
		{
			return false;
		}

		std::shared_lock<std::shared_mutex> lock(m_mtx_pool);
		const auto mIter = m_pool.find(id);
		if (mIter == m_pool.end())
		{
			return false;
		}
		return Send(mIter->second->m_pEvent, data, nLength);
	}

	bool CNetPool::Send(struct bufferevent* pEvent, const char* data, size_t nLength)
	{
		if ((nullptr == pEvent) || (nullptr == data) || (0 == nLength))
		{
			return false;
		}
		struct evbuffer* pBuffer = bufferevent_get_output(pEvent);
		if (nullptr == pBuffer)
		{
			return false;
		}

		return evbuffer_add(pBuffer, data, nLength) == 0;
	}

	bool CNetPool::SendRequest(net::_TyConnectionId id, const CRequest& request)
	{
		std::string strPayload;
		if (!request.Serialize(&strPayload))
		{
			return false;
		}
		auto ret = net::CFrameBuffer::Encode(strPayload);
		if (!ret.has_value())
		{
			return false;
		}

		std::shared_lock<std::shared_mutex> lock(m_mtx_pool);
		const auto mIter = m_pool.find(id);
		if (mIter == m_pool.end())
		{
			return false;
		}
		return Send(mIter->second->m_pEvent, ret->data(), ret->size());
	}

	// 读超时，客户端一定时间没有收到来自服务端的请求则认为断开
	bool CNetPool::SetReadTimeout(_TyConnectionId id, int nSeconds)
	{
		if (0 >= nSeconds)
		{
			return false;
		}
		std::shared_lock<std::shared_mutex> lock(m_mtx_pool);
		auto mIter = m_pool.find(id);
		if ((m_pool.end() == mIter) || (nullptr == mIter->second->m_pEvent))
		{
			return false;
		}
		timeval readTimeout{ nSeconds, 0 };
		return 0 == bufferevent_set_timeouts(mIter->second->m_pEvent, &readTimeout, nullptr);
	}

	std::size_t CNetPool::Count() const
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_pool);
		return m_pool.size();
	}

} // namespace net
