#ifndef XENGINE_STRATEGY_CTRADESERVICE_H
#define XENGINE_STRATEGY_CTRADESERVICE_H
#include "../trade/CBrokerClient.h"
#include "AccountInfo.h"
#include "ITradeService.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <map>
#include <set>
#include <unordered_set>
class CRequest;

class CTradeService final : public IStrategyOrderSink, public ITradingSnapshotProvider
{
	friend class CServiceRoutingTests;

public:
	using _TyOrderEventHandler = ITradeService::_TyOrderEventHandler;
	using _TyPushHandler = std::function<void(const CRequest&)>;
	using _TyAccountReadyHandler = std::function<void(const _TyAccount&)>;
	CTradeService() = default;
	~CTradeService();
	bool Initialize();
	void Stop();
	void SetOrderEventHandler(_TyOrderEventHandler&& handler);
	bool HandleTradingRequest(const CRequest& req);
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	bool HasAccount(const _TyAccount& accountIndex) const;
	bool IsConnected(const _TyAccount& accountIndex) const;
	bool BeginStrategy(const _TyAccount& accountIndex, _TyStrategyId strategyId);
	void EndStrategy(const _TyAccount& accountIndex, _TyStrategyId strategyId);
	bool HasActiveOrders(const _TyAccount& accountIndex) const;
	COrderSubmitResult Submit(const COrderIntent& intent) override;
	bool Cancel(_TyStrategyId strategyId, _TyOrderId orderId) override;
	CPositionSnapshot GetPosition(const CSecurity& security) const override;
	CAccountSnapshot GetAccount() const override;
	CPositionSnapshot GetPosition(const _TyAccount& accountIndex, const CSecurity& security) const;
	CAccountSnapshot GetAccount(const _TyAccount& accountIndex) const;
	const std::string& GetLastError() const;

private:
	std::shared_ptr<trade::ITradeSession> FindService(const _TyAccount& accountIndex) const;
	mutable std::mutex m_mtx_accounts;
	std::set<_TyAccount> m_disconnectingAccounts;
	std::map<_TyAccount, CTradeAccount> m_accounts;
	std::map<_TyAccount, std::unordered_set<_TyStrategyId>> m_runningStrategies;
	using _TyStrategyOrderIndex = std::pair<_TyStrategyId, _TyOrderId>;
	std::map<_TyStrategyOrderIndex, _TyAccount> m_orderAccounts;
	_TyOrderEventHandler m_orderHandler;
	std::string m_strLastError;
	std::shared_ptr<CBrokerClient> m_broker;
	using _TyTradeEventHandler = std::function<void(const CTradeEvent&)>;
	_TyTradeEventHandler m_tradeHandler;
	void OnEvent(const trade::CCountEvent& event);

public:
	void SetTradeEventHandler(_TyTradeEventHandler&& handler);
	std::vector<trade::COrder> GetOrders(const _TyAccount& accountIndex) const;
	void SetPushHandler(_TyPushHandler&& handler);
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);

private:
	_TyPushHandler m_pushHandler;
	_TyAccountReadyHandler m_accountReadyHandler;
	std::map<_TyAccount, std::uint64_t> m_lastEventSequence;
};
#endif
