#pragma once
#include "CConfiguredStrategy.h"

class CTakeProfitStopLossStrategy final : public CConfiguredStrategy
{
public:
	CTakeProfitStopLossStrategy() : CConfiguredStrategy(strategy::Kind::TakeProfitStopLoss)
	{
	}
};
