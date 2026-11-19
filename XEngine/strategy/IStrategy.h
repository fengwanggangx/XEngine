#ifndef XENGINE_STRATEGY_ISTRATEGY_H
#define XENGINE_STRATEGY_ISTRATEGY_H

#include "StrategyTypes.h"

class IStrategyContext;

// 定义策略初始化、启停及行情、委托、成交等事件回调接口。
class IStrategy
{
	public:
	virtual ~IStrategy() = default;

	virtual bool Initialize(IStrategyContext& context, const CStrategyConfig& config) = 0;
	virtual void OnStart() = 0;
	virtual void OnPause() = 0;
	virtual void OnStop() = 0;
	virtual bool OnAction(const std::string&)
	{
		return false;
	}
	virtual void OnHistory(const std::vector<_TyBarData>&)
	{
	}
	virtual void OnMarketState(bool)
	{
	}
	virtual int QuotePriority(const _TyQuoteData&) const
	{
		return 3;
	}

	virtual void OnQuote(const _TyQuoteData& data)
	{
	}
	virtual void OnDepth(const _TyDepthData& data)
	{
	}
	virtual void OnMarketTrade(const _TyMarketTradeData& data)
	{
	}
	virtual void OnBar(const _TyBarData& data)
	{
	}
	virtual void OnOrder(const COrderEvent& event)
	{
	}
	virtual void OnTrade(const CTradeEvent& event)
	{
	}
	virtual void OnTimer(const CTimerEvent& event)
	{
	}
};

#endif
