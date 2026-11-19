#pragma once
#include "CConfiguredStrategy.h"

// 按配置的止盈、止损条件监控持仓，并触发退出任务。
class CTakeProfitStopLossStrategy final : public CConfiguredStrategy
{
public:
	CTakeProfitStopLossStrategy() : CConfiguredStrategy(strategy::Kind::TakeProfitStopLoss)
	{
	}
};
