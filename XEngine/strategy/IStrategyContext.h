#ifndef XENGINE_STRATEGY_ISTRATEGYCONTEXT_H
#define XENGINE_STRATEGY_ISTRATEGYCONTEXT_H

#include "StrategyTypes.h"
#include "../trade/StrategyAccountView.h"

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
};

#endif
