#pragma once

#include <algorithm>
#include <concepts>
#include <iterator>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include "../common/utility.h"

namespace strategy
{
	enum class Kind
	{
		DualMaCross,
		TakeProfitStopLoss,
		TrailingTakeProfit,
		PriceBreakout
	};
	using _TyParameters = std::unordered_map<std::string, std::string>;

	inline bool ParseFixed(const std::string& text, int scale, std::int64_t& result)
	{
		if (text.empty() || (0 > scale) || (6 < scale) || (text.front() < '0') || (text.front() > '9'))
		{
			return false;
		}
		std::size_t dot = text.find('.');
		std::string whole = text.substr(0, dot);
		std::string fraction = std::string::npos == dot ? std::string() : text.substr(dot + 1);
		if ((fraction.size() > static_cast<std::size_t>(scale)) || ((std::string::npos != dot) && fraction.empty()))
		{
			return false;
		}
		if (!std::all_of(fraction.begin(), fraction.end(), [](char value)
		{
			return ('0' <= value) && ('9' >= value);
		}))
		{
			return false;
		}
		std::int64_t integer = 0;
		if (!utility::to_number(whole, integer) || (0 > integer))
		{
			return false;
		}
		fraction.append(static_cast<std::size_t>(scale) - fraction.size(), '0');
		std::int64_t decimal = 0;
		if (!fraction.empty() && !utility::to_number(fraction, decimal))
		{
			return false;
		}
		std::int64_t factor = 1;
		for (int index = 0; scale > index; ++index)
		{
			factor *= 10;
		}
		if (integer > ((std::numeric_limits<std::int64_t>::max)() - decimal) / factor)
		{
			return false;
		}
		result = integer * factor + decimal;
		return true;
	}

	inline std::string Value(const _TyParameters& parameters, const std::string& key, const std::string& fallback = {})
	{
		const auto mIter = parameters.find(key);
		return parameters.end() == mIter ? fallback : mIter->second;
	}

	// 保存价格或百分比阈值及其启用状态。
	struct CThreshold
	{
		bool m_enabled{ false };
		bool m_percent{ false };
		std::int64_t m_value{ 0 }; // Percent: parts per million; price: six decimal places.
	};

	// 解析并校验各类内置策略的交易数量、触发阈值和指标参数。
	struct CSettings
	{
		Kind m_kind{ Kind::DualMaCross };
		bool m_auto_rearm{ false };
		std::string m_buy_mode{ "quantity" };
		std::int64_t m_buy_value{ 100 };
		std::string m_sell_mode{ "all" };
		std::int64_t m_sell_value{ 0 };
		int m_short_window{ 5 };
		int m_long_window{ 20 };
		bool m_ema{ false };
		std::string m_bar_period{ "1m" };
		CThreshold m_take_profit;
		CThreshold m_stop_loss;
		CThreshold m_activation;
		CThreshold m_drawdown;
		std::int64_t m_trigger_price{ 0 };
		bool m_up{ true };
		bool m_buy{ true };

		static bool Read(const std::string& type, const _TyParameters& parameters, CSettings& settings, std::string& error)
		{
			CSettings parsed;
			if ("dual_ma_cross" == type)
			{
				parsed.m_kind = Kind::DualMaCross;
			}
			else if ("take_profit_stop_loss" == type)
			{
				parsed.m_kind = Kind::TakeProfitStopLoss;
			}
			else if ("trailing_take_profit" == type)
			{
				parsed.m_kind = Kind::TrailingTakeProfit;
			}
			else if ("price_breakout" == type)
			{
				parsed.m_kind = Kind::PriceBreakout;
			}
			else
			{
				error = "不支持的策略类型";
				return false;
			}
			std::string allowed = "|buy_mode|buy_value|sell_mode|sell_value|auto_rearm|";
			if (Kind::DualMaCross == parsed.m_kind)
			{
				allowed += "short_window|long_window|ma_type|bar_period|";
			}
			if (Kind::TakeProfitStopLoss == parsed.m_kind)
			{
				allowed += "take_profit_enabled|take_profit_mode|take_profit_value|stop_loss_enabled|stop_loss_mode|stop_loss_value|";
			}
			if (Kind::TrailingTakeProfit == parsed.m_kind)
			{
				allowed += "activation_mode|activation_value|drawdown_mode|drawdown_value|";
			}
			if (Kind::PriceBreakout == parsed.m_kind)
			{
				allowed += "trigger_price|direction|action|";
			}
			for (const auto& [key, value] : parameters)
			{
				if ((std::string::npos == allowed.find("|" + key + "|")) || value.empty())
				{
					error = "未知或空参数：" + key;
					return false;
				}
			}
			std::string rearm = Value(parameters, "auto_rearm", "0");
			if (("0" != rearm) && ("1" != rearm))
			{
				error = "auto_rearm 必须为 0 或 1";
				return false;
			}
			parsed.m_auto_rearm = "1" == rearm;
			parsed.m_buy_mode = Value(parameters, "buy_mode", "quantity");
			parsed.m_sell_mode = Value(parameters, "sell_mode", "all");
			if (("quantity" != parsed.m_buy_mode) && ("amount" != parsed.m_buy_mode) && ("funds_ratio" != parsed.m_buy_mode))
			{
				error = "买入模式无效";
				return false;
			}
			if (("all" != parsed.m_sell_mode) && ("quantity" != parsed.m_sell_mode) && ("position_ratio" != parsed.m_sell_mode))
			{
				error = "卖出模式无效";
				return false;
			}
			int buyScale = "quantity" == parsed.m_buy_mode ? 0 : ("amount" == parsed.m_buy_mode ? 2 : 4);
			if (!ParseFixed(Value(parameters, "buy_value", "100"), buyScale, parsed.m_buy_value) || (0 >= parsed.m_buy_value) || (("funds_ratio" == parsed.m_buy_mode) && (1000000 < parsed.m_buy_value)))
			{
				error = "买入数量、金额或比例无效";
				return false;
			}
			if ("all" != parsed.m_sell_mode)
			{
				if (!ParseFixed(Value(parameters, "sell_value"), "quantity" == parsed.m_sell_mode ? 0 : 4, parsed.m_sell_value) || (0 >= parsed.m_sell_value) || (("position_ratio" == parsed.m_sell_mode) && (1000000 < parsed.m_sell_value)))
				{
					error = "卖出数量或比例无效";
					return false;
				}
			}
			if (Kind::DualMaCross == parsed.m_kind)
			{
				if (!utility::to_number(Value(parameters, "short_window", "5"), parsed.m_short_window) || !utility::to_number(Value(parameters, "long_window", "20"), parsed.m_long_window) || (0 >= parsed.m_short_window) || (parsed.m_short_window >= parsed.m_long_window) || (100000 < parsed.m_long_window))
				{
					error = "均线窗口必须满足 1 <= short_window < long_window <= 100000";
					return false;
				}
				std::string algorithm = Value(parameters, "ma_type", "sma");
				parsed.m_bar_period = Value(parameters, "bar_period", "1m");
				if ((("sma" != algorithm) && ("ema" != algorithm)) || (("1m" != parsed.m_bar_period) && ("1d" != parsed.m_bar_period)))
				{
					error = "均线算法或 K 线周期无效";
					return false;
				}
				parsed.m_ema = "ema" == algorithm;
			}
			auto readThreshold = [&parameters, &error](const std::string& prefix, CThreshold& threshold, bool enabled, bool amount)
			{
				threshold.m_enabled = enabled;
				if (!enabled)
				{
					return true;
				}
				std::string mode = Value(parameters, prefix + "_mode", "percent");
				threshold.m_percent = "percent" == mode;
				if (!threshold.m_percent && ((amount ? "amount" : "price") != mode))
				{
					error = prefix + " 模式无效";
					return false;
				}
				if (!ParseFixed(Value(parameters, prefix + "_value"), threshold.m_percent ? 4 : 6, threshold.m_value) || (0 >= threshold.m_value) || (threshold.m_value > (std::numeric_limits<std::int64_t>::max)() - 1000000) || (threshold.m_percent && amount && (1000000 <= threshold.m_value)))
				{
					error = prefix + " 阈值无效";
					return false;
				}
				return true;
			};
			if (Kind::TakeProfitStopLoss == parsed.m_kind)
			{
				std::string profit = Value(parameters, "take_profit_enabled", "0");
				std::string loss = Value(parameters, "stop_loss_enabled", "1");
				if ((("0" != profit) && ("1" != profit)) || (("0" != loss) && ("1" != loss)) || (("0" == profit) && ("0" == loss)))
				{
					error = "至少启用一个止盈止损条件";
					return false;
				}
				if (!readThreshold("take_profit", parsed.m_take_profit, "1" == profit, false) || !readThreshold("stop_loss", parsed.m_stop_loss, "1" == loss, false))
				{
					return false;
				}
				if (parsed.m_stop_loss.m_percent && (1000000 <= parsed.m_stop_loss.m_value))
				{
					error = "止损比例必须小于 100%";
					return false;
				}
				if (parsed.m_take_profit.m_enabled && parsed.m_stop_loss.m_enabled && !parsed.m_take_profit.m_percent && !parsed.m_stop_loss.m_percent && (parsed.m_take_profit.m_value <= parsed.m_stop_loss.m_value))
				{
					error = "止损价必须低于止盈价";
					return false;
				}
			}
			if ((Kind::TrailingTakeProfit == parsed.m_kind) && (!readThreshold("activation", parsed.m_activation, true, false) || !readThreshold("drawdown", parsed.m_drawdown, true, true)))
			{
				return false;
			}
			if (Kind::PriceBreakout == parsed.m_kind)
			{
				std::string direction = Value(parameters, "direction", "up");
				std::string action = Value(parameters, "action", "buy");
				if ((("up" != direction) && ("down" != direction)) || (("buy" != action) && ("sell" != action)) || !ParseFixed(Value(parameters, "trigger_price"), 6, parsed.m_trigger_price) || (0 >= parsed.m_trigger_price))
				{
					error = "突破方向、动作或价格无效";
					return false;
				}
				parsed.m_up = "up" == direction;
				parsed.m_buy = "buy" == action;
			}
			settings = parsed;
			error.clear();
			return true;
		}
	};
} // namespace strategy
