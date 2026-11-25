#include "../log/CLogger.h"
#include "CNet.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <event2/event.h>

namespace net
{
	CNet::CNet()
	{
		m_net = event_base_new();
	}

	CNet::~CNet()
	{
		Release();
	}

	void CNet::Release()
	{
		if (nullptr != m_net)
		{
			if (m_running)
			{
				ShutDown();
			}
			event_base_free(m_net);
			m_net = nullptr;
		}
	}

	struct event_base* CNet::GetNet()
	{
		return m_net;
	}

	void CNet::ShutDown()
	{
		if (nullptr != m_net)
		{
			// A scheduled exit also applies if dispatch has not entered the loop yet.
			event_base_loopexit(m_net, nullptr);
			m_running.store(false);
		}
	}

	bool CNet::Start(bool bRealTime)
	{
		if ((nullptr == m_net) || m_running.load())
		{
			return false;
		}
		m_running.store(true);
		bool success = true;
		if (bRealTime)
		{
			int nRet = event_base_dispatch(m_net);
			if (-1 == nRet)
			{
				success = false;
				CLogger::Write(Level::Error, "network", "事件循环错误");
			}
		}
		else
		{
			while (m_running)
			{
				int nRet = event_base_loop(m_net, EVLOOP_NONBLOCK);
				if (-1 == nRet)
				{
					CLogger::Write(Level::Error, "network", "事件循环错误");
					success = false;
					break;
				}
				std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		}

		m_running.store(false);
		return success;
	}
} // namespace net
