#ifndef __CNET_H__
#define __CNET_H__
#include <atomic>
#include <event2/util.h>

struct event_base;

namespace net
{
	class CNet
	{
	public:
		CNet();
		virtual ~CNet();

	public:
		bool Start(bool bRealTime);
		void ShutDown();

	protected:
		struct event_base* GetNet();

	private:
		void Release();

	private:
		struct event_base* m_net{ nullptr };
		std::atomic_bool m_running{ false };
	};
} // namespace net
#endif
