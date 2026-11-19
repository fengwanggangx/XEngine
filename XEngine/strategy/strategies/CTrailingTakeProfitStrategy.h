#pragma once
#include "CConfiguredStrategy.h"

// 跟踪持仓价格高点，在满足激活及回撤条件时触发退出任务。
class CTrailingTakeProfitStrategy final : public CConfiguredStrategy
{
public:
	CTrailingTakeProfitStrategy() : CConfiguredStrategy(strategy::Kind::TrailingTakeProfit)
	{
	}
};
