#pragma once
#include "CConfiguredStrategy.h"

// 监控价格突破条件，并按配置方向和数量触发交易。
class CPriceBreakoutStrategy final : public CConfiguredStrategy
{
public:
	CPriceBreakoutStrategy() : CConfiguredStrategy(strategy::Kind::PriceBreakout)
	{
	}
};
