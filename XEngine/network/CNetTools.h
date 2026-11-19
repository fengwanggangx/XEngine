#ifndef __CNETTOOLS_H__
#define __CNETTOOLS_H__
#include <vector>
#include <memory>
#include <event2/util.h>
#include "CNet.h"
#include "common_net.h"

struct bufferevent;
class CRequest;

namespace net
{
	net::_TyConnectionId GetConnectionId(struct bufferevent* pEvent);
	std::size_t BufferEventReader(struct bufferevent* pEvent, std::vector<char>& buffer);
	std::size_t RequestFromBuffer(std::vector<std::unique_ptr<CRequest>>& reqs, struct bufferevent* pEvent);

	bool SendRequest(net::_TyConnectionId id, const CRequest& request);
	bool SendRequest(struct bufferevent* pEvent, const CRequest& request);

	void SetError(CRequest& request, int code, const std::string& message);
	void SetError(CRequest& response, const CRequest& request, int code, const std::string& message);
	void SendError(net::_TyConnectionId id, const CRequest& request, int code, const std::string& message);
} // namespace net
#endif
