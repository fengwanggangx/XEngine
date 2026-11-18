#pragma once
#include "CConfiguredStrategy.h"

class CPriceBreakoutStrategy final : public CConfiguredStrategy
{
public:
	CPriceBreakoutStrategy() : CConfiguredStrategy(strategy::Kind::PriceBreakout)
	{
	}
};
