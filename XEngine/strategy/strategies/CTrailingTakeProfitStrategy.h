#pragma once
#include "CConfiguredStrategy.h"

class CTrailingTakeProfitStrategy final : public CConfiguredStrategy
{
public:
	CTrailingTakeProfitStrategy() : CConfiguredStrategy(strategy::Kind::TrailingTakeProfit)
	{
	}
};
