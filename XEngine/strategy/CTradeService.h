#ifndef XENGINE_STRATEGY_CTRADESERVICE_H
#define XENGINE_STRATEGY_CTRADESERVICE_H
#include "../trade/CBrokerClient.h"
#include "AccountInfo.h"
#include "ITradeService.h"
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
class CRequest;
class CSession;

class CTradeService final : public IStrategyOrderSink, public ITradingSnapshotProvider
{
	public:
	using _TyOrderEventHandler = ITradeService::_TyOrderEventHandler;
	using _TyPushHandler = std::function<void(const CRequest&)>;
	using _TyAccountReadyHandler = std::function<void(std::uint64_t)>;
	CTradeService() = default;
	~CTradeService();
	bool Initialize(CSession& session);
	void Stop();
	void SetOrderEventHandler(_TyOrderEventHandler&& handler);
	bool HandleTradingRequest(const CRequest& req);
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	bool HasAccount(std::uint64_t accountId) const;
	bool IsConnected(std::uint64_t accountId) const;
	bool BeginStrategy(std::uint64_t accountId, _TyStrategyId strategyId);
	void EndStrategy(std::uint64_t accountId, _TyStrategyId strategyId);
	bool HasActiveOrders(std::uint64_t accountId) const;
	COrderSubmitResult Submit(const COrderIntent& intent) override;
	bool Cancel(_TyStrategyId strategyId, _TyOrderId orderId) override;
	CPositionSnapshot GetPosition(const CSecurity& security) const override;
	CAccountSnapshot GetAccount() const override;
	CPositionSnapshot GetPosition(std::uint64_t accountId, const CSecurity& security) const;
	CAccountSnapshot GetAccount(std::uint64_t accountId) const;
	const std::string& GetLastError() const;

	private:
	std::shared_ptr<trade::ITradingGateway> FindService(std::uint64_t accountId) const;
	mutable std::mutex m_mtx_accounts;
	std::mutex m_mtx_operations;
	std::unordered_map<std::uint64_t, CTradeAccount> m_accounts;
	std::unordered_map<_TyOrderId, std::uint64_t> m_orderAccounts;
	_TyOrderEventHandler m_orderHandler;
	std::string m_strLastError;
	std::shared_ptr<CBrokerClient> m_broker;
	using _TyTradeEventHandler = std::function<void(const CTradeEvent&)>;
	_TyTradeEventHandler m_tradeHandler;
	void OnEvent(const trade::CEvent& event);

	public:
	void SetTradeEventHandler(_TyTradeEventHandler&& handler);
	std::vector<trade::COrder> GetOrders(std::uint64_t accountId) const;
	void SetPushHandler(_TyPushHandler&& handler);
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);

	private:
	_TyPushHandler m_pushHandler;
	_TyAccountReadyHandler m_accountReadyHandler;
	std::unordered_map<std::uint64_t, std::uint64_t> m_lastEventSequence;
};
#endif
