#ifndef __CTCPSERVER_H__
#define __CTCPSERVER_H__

#include <memory>
#include <vector>
#include <functional>
#include "CNet.h"
#include "common_net.h"
#include "CNetRouter.h"
template <bool bAsyn, class _Ty, class _TyHandler>
class CDistributor;

class CRequest;

namespace net
{
	class CTcpServer final : public CNet, public CNetRouter<CTcpServer>
	{
		using _TyHandler = std::function<int(const CNetEvent&)>;
		using _TyDistributor = CDistributor<true, std::vector<CNetEvent>, _TyHandler>;

	public:
		explicit CTcpServer(int nPort);
		~CTcpServer() override;

	public:
		void OnListenerError(struct evconnlistener* pListener);

		int Initialize();
		void Drain();
		void ClearHandlers();

		void RegisterHandler(_TyHandler&& func);

	public:
		void OnConnAccept(struct evconnlistener* pListener, _TyConnectionId id, struct sockaddr* pAddr, int nLength) override;
		std::size_t OnRead(struct bufferevent* pEvent) override;
		void OnEvent(struct bufferevent* pEvent, short events) override;

	private:
		int m_nPort{ -1 };
		std::unique_ptr<_TyDistributor> m_dispatcher;
	};
} // namespace net
#endif
