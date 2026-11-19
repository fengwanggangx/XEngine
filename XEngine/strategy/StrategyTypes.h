#ifndef XENGINE_STRATEGY_STRATEGYTYPES_H
#define XENGINE_STRATEGY_STRATEGYTYPES_H

#include "../trade/TradingTypes.h"
#include "../request/v1/market.pb.h"

#include <chrono>
#include <cstdint>
#include "../request/AccountIdentity.h"
#include <string>
#include <unordered_map>
#include <map>
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

// 保存策略身份、类型、参数、账户、行情订阅及运行配置。
struct CStrategyConfig
{
	_TyAccount m_account{};
	_TyStrategyId m_id{ 0 };
	std::string m_str_type;
	std::uint64_t m_revision{ 1 };
	std::string m_str_name;
	std::unordered_map<std::string, std::string> m_params;
	std::vector<CQuoteInfo> m_subscriptions;
	std::size_t m_event_queue_limit{ 4096 };
	bool m_b_auto_start{ false };

	bool IsValid() const;
};

// 携带策略定时回调的事件数据。
struct CTimerEvent
{
	_TyTimerId m_timer_id{ 0 };
	std::chrono::steady_clock::time_point m_fire_time;
	bool m_zero_position{ false };
};

// 汇总策略生命周期、业务状态、错误及运行指标，供查询和状态通知使用。
struct CStrategySnapshot
{
	_TyStrategyId m_id{ 0 };
	std::string m_str_type;
	std::string m_str_name;
	StrategyState m_state{ StrategyState::Created };
	bool m_b_market_available{ false };
	std::size_t m_queue_length{ 0 };
	std::size_t m_active_order_count{ 0 };
	std::uint64_t m_last_event_sequence{ 0 };
	std::uint64_t m_dropped_market_events{ 0 };
	std::int64_t m_last_callback_duration_us{ 0 };
	std::int64_t m_max_callback_duration_us{ 0 };
	std::string m_str_last_error;
	std::string m_business_state;
	std::string m_details_json;
};

#endif
