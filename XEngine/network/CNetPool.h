#ifndef __CNETPOOL_H__
#define __CNETPOOL_H__

#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <memory>
#include <optional>
#include <vector>
#include "../common/ISingleton.h"
#include "CNet.h"
#include "event2/bufferevent.h"
#include "common_net.h"

namespace net
{
	enum class RecvFrameResult
	{
		Ok,
		ConnectionNotFound,
		ProtocolError
	};

	struct CNetInfo;

	class CNetPool final : public ISingleton<CNetPool>
	{
		DECLARE_SINGLE_DFAULT(CNetPool)

	public:
		std::size_t Count() const;

		std::pair<RecvFrameResult, std::vector<std::string>> GetRecvFrames(_TyConnectionId id, struct bufferevent* pEvent, const char* data, std::size_t nLength);

		net::_TyConnectionId CloseAConnection(_TyConnectionId id);
		net::_TyConnectionId CloseAConnection(struct bufferevent* pEvent);

		bool Send(_TyConnectionId id, const char* data, size_t nLength);
		bool SendRequest(net::_TyConnectionId id, const CRequest& request);
		bool SetReadTimeout(_TyConnectionId id, int nSeconds);

		struct bufferevent* RegisterConnect(_TyConnectionId id, struct event_base* pNet, struct sockaddr* pAddr, int nLength, bufferevent_data_cb readcb, bufferevent_data_cb writecb, bufferevent_event_cb eventcb, void* cbarg);

		_TyConnectionId RegisterAConnection(struct bufferevent* pEvent);
		struct bufferevent* RegisterAConnection(_TyConnectionId id, struct bufferevent* pEvent, struct sockaddr_storage* pAddr);

	private:
		bool CloseAConnection(CNetInfo& info);
		bool RegisterAConnection(_TyConnectionId id, struct bufferevent* pEvent, struct sockaddr* pAddr);

		bool Send(struct bufferevent* pEvent, const char* data, size_t nLength);

	private:
		mutable std::shared_mutex m_pool_mutex;
		std::unordered_map<_TyConnectionId, std::unique_ptr<CNetInfo>> m_pool;
	};
} // namespace net
#endif
