#ifndef XENGINE_STRATEGY_CTRADESERVICE_H
#define XENGINE_STRATEGY_CTRADESERVICE_H
#include "../trade/CBrokerClient.h"
#include "AccountInfo.h"
#include "ITradeService.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <map>
#include <unordered_set>
class CRequest;
class CSession;

class CTradeService final : public IStrategyOrderSink, public ITradingSnapshotProvider
{
public:
	using _TyOrderEventHandler = ITradeService::_TyOrderEventHandler;
	using _TyPushHandler = std::function<void(const CRequest&)>;
	using _TyAccountReadyHandler = std::function<void(const _TyAccountIndex&)>;
	CTradeService() = default;
	~CTradeService();
	bool Initialize(CSession& session);
	void Stop();
	void SetOrderEventHandler(_TyOrderEventHandler&& handler);
	bool HandleTradingRequest(const CRequest& req);
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	bool HasAccount(const _TyAccountIndex& accountIndex) const;
	bool IsConnected(const _TyAccountIndex& accountIndex) const;
	bool BeginStrategy(const _TyAccountIndex& accountIndex, _TyStrategyId strategyId);
	void EndStrategy(const _TyAccountIndex& accountIndex, _TyStrategyId strategyId);
	bool HasActiveOrders(const _TyAccountIndex& accountIndex) const;
	COrderSubmitResult Submit(const COrderIntent& intent) override;
	bool Cancel(_TyStrategyId strategyId, _TyOrderId orderId) override;
	CPositionSnapshot GetPosition(const CSecurity& security) const override;
	CAccountSnapshot GetAccount() const override;
	CPositionSnapshot GetPosition(const _TyAccountIndex& accountIndex, const CSecurity& security) const;
	CAccountSnapshot GetAccount(const _TyAccountIndex& accountIndex) const;
	const std::string& GetLastError() const;

private:
	std::shared_ptr<trade::ITradeCounter> FindService(const _TyAccountIndex& accountIndex) const;
	mutable std::mutex m_mtx_accounts;
	std::mutex m_mtx_operations;
	std::map<_TyAccountIndex, CTradeAccount> m_accounts;
	std::map<_TyAccountIndex, std::unordered_set<_TyStrategyId>> m_runningStrategies;
	using _TyStrategyOrderIndex = std::pair<_TyStrategyId, _TyOrderId>;
	std::map<_TyStrategyOrderIndex, _TyAccountIndex> m_orderAccounts;
	_TyOrderEventHandler m_orderHandler;
	std::string m_strLastError;
	std::shared_ptr<CBrokerClient> m_broker;
	using _TyTradeEventHandler = std::function<void(const CTradeEvent&)>;
	_TyTradeEventHandler m_tradeHandler;
	void OnEvent(const trade::CCountEvent& event);

public:
	void SetTradeEventHandler(_TyTradeEventHandler&& handler);
	std::vector<trade::COrder> GetOrders(const _TyAccountIndex& accountIndex) const;
	void SetPushHandler(_TyPushHandler&& handler);
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);

private:
	_TyPushHandler m_pushHandler;
	_TyAccountReadyHandler m_accountReadyHandler;
	std::map<_TyAccountIndex, std::uint64_t> m_lastEventSequence;
};
#endif
