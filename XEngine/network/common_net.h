#ifndef __COMMON_NET_H__
#define __COMMON_NET_H__
#include <event2/util.h>
#include <netinet/in.h>
#include <string>
#include <memory>

class CRequest;

namespace net
{
	using _TyConnectionId = evutil_socket_t;
	enum class em_event
	{
		unknown = 0,
		connected,
		request,
		disconnected,
		error,
		timeout
	};

	struct CNetEvent
	{
		explicit CNetEvent(em_event event);
		explicit CNetEvent(em_event event, _TyConnectionId id);
		~CNetEvent();
		CNetEvent(CNetEvent&&) noexcept;
		CNetEvent& operator=(CNetEvent&&) noexcept;
		CNetEvent(const CNetEvent&) = delete;
		CNetEvent& operator=(const CNetEvent&) = delete;

		em_event m_event{ em_event::unknown };
		_TyConnectionId m_connection_id{ -1 };
		std::unique_ptr<CRequest> m_request{ nullptr };
		int m_error{ -1 };
	};

	bool IsThreadEnable();
	void EnvInitialize();
	bool FmtAddress(struct ::sockaddr_in& addr, int nPort, const std::string& strAddr = "");

	std::string ParseSockAddr(std::string& strAddr, int& nPort, const struct sockaddr& addr);
	std::string ParseSockAddr(std::string& strAddr, int& nPort, const struct sockaddr_storage& addr);

	bool CheckSockAddress(struct sockaddr* pAddr, int nLength);
	bool SockAddrSafeCopy(struct sockaddr& dst, const struct sockaddr& src);
	bool SockAddrSafeCopy(struct sockaddr_storage& dst, const struct sockaddr& src);
} // namespace net

#endif
