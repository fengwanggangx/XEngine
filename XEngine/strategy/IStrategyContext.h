#ifndef XENGINE_STRATEGY_ISTRATEGYCONTEXT_H
#define XENGINE_STRATEGY_ISTRATEGYCONTEXT_H

#include "StrategyTypes.h"
#include "../trade/StrategyAccountView.h"
#include "CStrategyExecution.h"

class IStrategyContext
{
	public:
	virtual ~IStrategyContext() = default;

	virtual COrderSubmitResult SubmitOrder(const COrderIntent& intent) = 0;
	virtual bool CancelOrder(_TyOrderId orderId) = 0;
	virtual CPositionSnapshot GetPosition(const CSecurity& security) const = 0;
	virtual CAccountSnapshot GetAccount() const = 0;
	virtual void WriteLog(LogLevel level, const std::string& strMessage) = 0;
	virtual bool IsMarketAvailable() const = 0;
	virtual CStrategyAccountView GetTradingSnapshot(const CSecurity& security) const
	{
		static_cast<void>(security);
		return {};
	}
	virtual std::optional<trade::CExecutionRule> GetRules(const CSecurity&)
	{
		return std::nullopt;
	}
	virtual std::string LoadCheckpoint() const
	{
		return {};
	}
	virtual bool SaveCheckpoint(const std::string&, bool)
	{
		return false;
	}
	virtual std::uint64_t AllocateSignal()
	{
		return 0;
	}
	virtual std::optional<CExecutionRecord> FindExecution(std::uint64_t) const
	{
		return std::nullopt;
	}
	virtual std::optional<CExecutionRecord> FindSignal(std::uint64_t) const
	{
		return std::nullopt;
	}
	virtual bool ClaimExit()
	{
		return false;
	}
	virtual bool ReleaseExit(bool)
	{
		return false;
	}
	virtual bool CancelConflicts(bool = false)
	{
		return false;
	}
	virtual bool RequestHistory(std::int64_t)
	{
		return false;
	}
	virtual std::int64_t AvailableFunds(const CStrategyAccountView&) const
	{
		return 0;
	}
	virtual void PublishBusiness(const std::string&, const std::string&)
	{
	}
	virtual std::int64_t NowMs() const
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
	}
};

#endif
