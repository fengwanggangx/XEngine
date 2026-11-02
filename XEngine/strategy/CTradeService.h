#ifndef XENGINE_STRATEGY_CTRADESERVICE_H
#define XENGINE_STRATEGY_CTRADESERVICE_H
#include "../trade/CBrokerClient.h"
#include "AccountInfo.h"
#include "../trade/TradingTypes.h"
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <map>
#include <set>
class CRequest;

class CTradeService final
{
	friend class CServiceRoutingTests;

public:
	using _TyEventHandler = std::function<void(const trade::CCountEvent&)>;
	using _TyPushHandler = std::function<void(const CRequest&)>;
	using _TyAccountReadyHandler = std::function<void(const _TyAccount&)>;
	CTradeService() = default;
	~CTradeService();
	bool Initialize();
	void Stop();
	void SetEventHandler(_TyEventHandler&& handler);
	bool HandleTradingRequest(const CRequest& req);
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	bool HasAccount(const _TyAccount& accountIndex) const;
	bool IsConnected(const _TyAccount& accountIndex) const;
	bool AcquireAccount(const _TyAccount& accountIndex);
	void ReleaseAccount(const _TyAccount& accountIndex);
	bool HasActiveOrders(const _TyAccount& accountIndex) const;
	trade::CResult SendRequest(trade::CReqParam request, CBrokerClient::_TyReplyHandler handler);
	CPositionSnapshot GetPosition(const _TyAccount& accountIndex, const CSecurity& security) const;
	CAccountSnapshot GetAccount(const _TyAccount& accountIndex) const;
	const std::string& GetLastError() const;

private:
	bool CanUseAccount(const _TyAccount& accountIndex) const;
	mutable std::mutex m_mtx_accounts;
	std::set<_TyAccount> m_disconnectingAccounts;
	std::map<_TyAccount, CTradeAccountInfo> m_accounts;
	std::map<_TyAccount, std::size_t> m_accountUsers;
	_TyEventHandler m_eventHandler;
	std::string m_strLastError;
	std::unique_ptr<CBrokerClient> m_broker;
	void OnEvent(const trade::CCountEvent& event);

public:
	std::vector<trade::COrder> GetOrders(const _TyAccount& accountIndex) const;
	void SetPushHandler(_TyPushHandler&& handler);
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);

private:
	_TyPushHandler m_pushHandler;
	_TyAccountReadyHandler m_accountReadyHandler;
	std::map<_TyAccount, std::uint64_t> m_lastEventSequence;
};
#endif
