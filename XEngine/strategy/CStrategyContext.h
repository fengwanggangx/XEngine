#ifndef XENGINE_STRATEGY_CSTRATEGYCONTEXT_H
#define XENGINE_STRATEGY_CSTRATEGYCONTEXT_H

#include "IStrategyContext.h"

#include <atomic>
#include <set>

class CStrategyService;
class CTradeService;

class CStrategyContext final : public IStrategyContext
{
public:
	CStrategyContext(_TyStrategyId strategyId, const _TyAccount& account, CStrategyService* pStrategyService, CTradeService* pSnapshotProvider);

	COrderSubmitResult SubmitOrder(const COrderIntent& intent) override;
	bool CancelOrder(_TyOrderId orderId) override;
	CPositionSnapshot GetPosition(const CSecurity& security) const override;
	CAccountSnapshot GetAccount() const override;
	void WriteLog(LogLevel level, const std::string& strMessage) override;
	bool IsMarketAvailable() const override;
	CStrategyAccountView GetTradingSnapshot(const CSecurity& security) const override;

	std::optional<trade::CExecutionRule> GetRules(const CSecurity& security) override;
	std::string LoadCheckpoint() const override;
	bool SaveCheckpoint(const std::string& data, bool protect) override;
	std::uint64_t AllocateSignal() override;
	std::optional<CExecutionRecord> FindExecution(std::uint64_t clientId) const override;
	std::optional<CExecutionRecord> FindSignal(std::uint64_t signalId) const override;
	bool ClaimExit() override;
	bool ReleaseExit(bool unlock) override;
	bool CancelConflicts(bool reset = false) override;
	bool RequestHistory(std::int64_t begin) override;
	std::int64_t AvailableFunds(const CStrategyAccountView& view) const override;
	void PublishBusiness(const std::string& phase, const std::string& details) override;

	void Disable();

private:
	_TyStrategyId m_id{ 0 };
	CStrategyService* m_pStrategyService{ nullptr };
	_TyAccount m_account{};
	CTradeService* m_pSnapshotProvider{ nullptr };
	std::atomic_bool m_bEnabled{ true };
	std::set<std::uint64_t> m_cancelled;
};

#endif
