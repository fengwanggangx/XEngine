#ifndef XENGINE_STRATEGY_STRATEGYTYPES_H
#define XENGINE_STRATEGY_STRATEGYTYPES_H

#include "../trade/TradingTypes.h"
#include "../request/v1/market.pb.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using _TyStrategyId = std::uint64_t;
using _TySignalId = std::uint64_t;
using _TyOrderId = std::uint64_t;
using _TyClientOrderId = std::uint64_t;
using _TyTimerId = std::uint64_t;

using _TyQuoteData = hqmarket::market::v1::QuoteData;
using _TyDepthData = hqmarket::market::v1::DepthData;
using _TyMarketTradeData = hqmarket::market::v1::TradeData;
using _TyBarData = hqmarket::market::v1::BarData;

enum class StrategyState
{
	Created,
	Initialized,
	Starting,
	Running,
	Pausing,
	Paused,
	Stopping,
	Stopped,
	Faulted
};

enum class LogLevel
{
	Debug,
	Info,
	Warning,
	Error
};

struct CStrategyConfig
{
	std::uint64_t m_accountId{ 1 };
	_TyStrategyId m_strategyId{ 0 };
	std::string m_strType;
	std::string m_strName;
	std::unordered_map<std::string, std::string> m_parameters;
	std::vector<CQuoteInfo> m_subscriptions;
	std::size_t m_eventQueueLimit{ 4096 };
	bool m_bAutoStart{ false };

	bool IsValid() const;
};

struct CTimerEvent
{
	_TyTimerId m_timerId{ 0 };
	std::chrono::steady_clock::time_point m_fireTime;
};

struct CStrategySnapshot
{
	_TyStrategyId m_strategyId{ 0 };
	std::string m_strType;
	std::string m_strName;
	StrategyState m_state{ StrategyState::Created };
	bool m_bMarketAvailable{ false };
	std::size_t m_queueLength{ 0 };
	std::size_t m_activeOrderCount{ 0 };
	std::uint64_t m_lastEventSequence{ 0 };
	std::uint64_t m_droppedMarketEvents{ 0 };
	std::int64_t m_lastCallbackDurationUs{ 0 };
	std::int64_t m_maxCallbackDurationUs{ 0 };
	std::string m_strLastError;
};

#endif
