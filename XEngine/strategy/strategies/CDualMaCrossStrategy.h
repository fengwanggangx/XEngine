#pragma once
#include "CConfiguredStrategy.h"

class CDualMaCrossStrategy final : public CConfiguredStrategy
{
public:
	explicit CDualMaCrossStrategy(CIndicator& indicator);
};
