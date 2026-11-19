#pragma once
#include "CConfiguredStrategy.h"

// 实现基于已完成 K 线的双均线交叉策略，支持历史预热及 SMA、EMA 指标。
class CDualMaCrossStrategy final : public CConfiguredStrategy
{
public:
	explicit CDualMaCrossStrategy(CIndicator& indicator);
};
