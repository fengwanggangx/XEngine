#include "CConfiguredStrategy.h"
#include "../IStrategyContext.h"
#include "../../indicator/CIndicator.h"
#include "../../trade/OrderRules.h"
#include "../../common/numeric.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace
{
	std::int64_t Power(int scale)
	{
		std::int64_t value = 1;
		for (int index = 0; scale > index; ++index)
		{
			value *= 10;
		}
		return value;
	}

	int CompareRatio(std::int64_t a, std::int64_t b, std::int64_t c, std::int64_t d)
	{
		bool reverse = false;
		while (true)
		{
			std::int64_t left = a / b;
			std::int64_t right = c / d;
			if (left != right)
			{
				return (left < right ? -1 : 1) * (reverse ? -1 : 1);
			}
			a %= b;
			c %= d;
			if ((0 == a) || (0 == c))
			{
				return (a == c ? 0 : (0 == a ? -1 : 1)) * (reverse ? -1 : 1);
			}
			std::swap(a, b);
			std::swap(c, d);
			reverse = !reverse;
		}
	}

	std::int64_t Rounded(std::int64_t quantity, std::int64_t minimum, std::int64_t step)
	{
		return ((0 >= minimum) || (0 >= step) || (quantity < minimum)) ? 0 : minimum + (quantity - minimum) / step * step;
	}

} // namespace

CConfiguredStrategy::CConfiguredStrategy(strategy::Kind kind) : m_kind(kind)
{
}

bool CConfiguredStrategy::ValidateParameters(const CStrategyConfig& config, std::string& error)
{
	strategy::CSettings settings;
	if (!strategy::CSettings::Read(config.m_strType, config.m_params, settings, error))
	{
		return false;
	}
	if (config.m_subscriptions.empty())
	{
		error = "必须指定一个股票标的";
		return false;
	}
	const CSecurity& security = config.m_subscriptions.front().m_security;
	if (((Exchange::sse != security.m_market) && (Exchange::szse != security.m_market) && (Exchange::bse != security.m_market)) || (6 != security.m_strCode.size()) || !std::all_of(security.m_strCode.begin(), security.m_strCode.end(), [](char value)
	{
		return ('0' <= value) && ('9' >= value);
	}))
	{
		error = "仅支持沪深北股票";
		return false;
	}
	bool quote = false, bar = false;
	for (const auto& subscription : config.m_subscriptions)
	{
		if (security != subscription.m_security)
		{
			error = "一个策略实例只能绑定一个标的";
			return false;
		}
		if (Channel::quote == subscription.m_channel)
		{
			quote = true;
		}
		else if ((strategy::Kind::DualMaCross == settings.m_kind) && (subscription.m_channel == ("1m" == settings.m_barPeriod ? Channel::bar_1m : Channel::bar_1d)))
		{
			bar = true;
		}
		else
		{
			error = "策略行情通道与类型或周期不一致";
			return false;
		}
	}
	if (!quote || ((strategy::Kind::DualMaCross == settings.m_kind) && !bar))
	{
		error = "缺少报价或对应 K 线订阅";
		return false;
	}
	return true;
}

bool CConfiguredStrategy::Initialize(IStrategyContext& context, const CStrategyConfig& config)
{
	std::string error;
	if (!ValidateParameters(config, error) || !strategy::CSettings::Read(config.m_strType, config.m_params, m_settings, error) || (m_kind != m_settings.m_kind))
	{
		context.WriteLog(LogLevel::Error, error);
		return false;
	}
	m_context = &context;
	m_config = config;
	m_security = config.m_subscriptions.front().m_security;
	std::ostringstream signature;
	signature << config.m_strType << ' ' << EncodeAccount(config.m_account) << ' ' << m_security.String();
	std::map<std::string, std::string> ordered(config.m_params.begin(), config.m_params.end());
	for (const auto& [key, value] : ordered)
	{
		signature << ' ' << std::quoted(key) << ' ' << std::quoted(value);
	}
	m_signature = signature.str();
	std::string checkpoint = context.LoadCheckpoint();
	if (!checkpoint.empty())
	{
		Restore(checkpoint);
	}
	return true;
}

bool CConfiguredStrategy::Protection() const
{
	return (strategy::Kind::TakeProfitStopLoss == m_kind) || (strategy::Kind::TrailingTakeProfit == m_kind);
}

void CConfiguredStrategy::Persist(bool protect)
{
	if (m_feedingHistory)
	{
		return;
	}
	std::ostringstream data;
	data << std::setprecision(17) << std::quoted(m_signature) << ' ' << std::quoted(m_phase) << ' ' << std::quoted(m_reason) << ' ' << m_task << ' ' << m_taskPaused << ' ' << m_tracking << ' ' << m_seenZero << ' ' << m_highest << ' ' << m_lastPosition << ' ' << m_epoch << ' ' << m_target << ' ' << m_done << ' ' << m_pending << ' ' << m_submittingSignal << ' ' << std::quoted(m_confirmedBasis) << ' ' << std::quoted(m_barBasis) << ' ' << m_seedBegin << ' ' << m_lastBar << ' ' << m_barVersion << ' ' << m_samples << ' ' << m_relation << ' ' << m_shortEma << ' ' << m_longEma << ' ' << m_prices.size();
	for (double price : m_prices)
	{
		data << ' ' << price;
	}
	if (!m_context->SaveCheckpoint(data.str(), protect))
	{
		throw std::runtime_error("Cannot persist strategy checkpoint; trading stopped");
	}
}

void CConfiguredStrategy::Restore(const std::string& text)
{
	std::istringstream data(text);
	std::string signature;
	std::size_t count = 0;
	if (!(data >> std::quoted(signature) >> std::quoted(m_phase) >> std::quoted(m_reason) >> m_task >> m_taskPaused >> m_tracking >> m_seenZero >> m_highest >> m_lastPosition >> m_epoch >> m_target >> m_done >> m_pending >> m_submittingSignal >> std::quoted(m_confirmedBasis) >> std::quoted(m_barBasis) >> m_seedBegin >> m_lastBar >> m_barVersion >> m_samples >> m_relation >> m_shortEma >> m_longEma >> count) || (100000 < count) || (0 > m_done) || (0 > m_highest))
	{
		throw std::runtime_error("Invalid strategy checkpoint");
	}
	for (std::size_t index = 0; count > index; ++index)
	{
		double value = 0;
		if (!(data >> value) || !std::isfinite(value) || (0 >= value))
		{
			throw std::runtime_error("Invalid persisted indicator input");
		}
		m_prices.emplace_back(value);
	}
	if (signature != m_signature)
	{
		if (m_task || (0 != m_pending))
		{
			throw std::runtime_error("Cannot replace an unresolved strategy configuration");
		}
		m_prices.clear();
		m_samples = 0;
		m_relation = 0;
		m_tracking = false;
		m_phase = "待处理";
		m_reason = "配置已变更，请重新布防";
	}
	else if (Protection() && !m_task && (0 < m_lastPosition) && ("已完成" != m_phase))
	{
		m_phase = "待处理";
		m_reason = "重启后的持仓周期待确认，请重新布防";
	}
	if ((0 == m_pending) && (0 != m_submittingSignal))
	{
		std::optional<CExecutionRecord> record = m_context->FindSignal(m_submittingSignal);
		if (record.has_value())
		{
			m_pending = record->m_intent.m_clientOrderId;
		}
		else
		{
			m_phase = "待处理";
			m_reason = "提交前中断，尚未建立委托，请重新布防";
		}
		m_submittingSignal = 0;
	}
}

void CConfiguredStrategy::SetPhase(const std::string& phase, const std::string& reason)
{
	bool changed = (phase != m_phase) || (reason != m_reason);
	m_phase = phase;
	m_reason = reason;
	if (changed)
	{
		Persist(m_task);
	}
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("reason");
	writer.String(m_reason.c_str());
	writer.Key("highest_price_6");
	writer.Int64(m_highest);
	writer.Key("target_quantity");
	writer.Int64(m_target);
	writer.Key("filled_quantity");
	writer.Int64(m_done);
	writer.Key("client_order_id");
	writer.Uint64(m_pending);
	writer.Key("task_paused");
	writer.Bool(m_taskPaused);
	writer.EndObject();
	m_context->PublishBusiness(m_phase, std::string(buffer.GetString(), buffer.GetSize()));
}

void CConfiguredStrategy::OnStart()
{
	m_running = true;
	m_previous = 0;
	m_quoteTime = 0;
	if (strategy::Kind::DualMaCross == m_kind)
	{
		m_historyReady = false;
		m_lastHistoryPoll = m_context->NowMs();
		m_context->RequestHistory(m_seedBegin);
	}
	SetPhase(m_phase, m_reason);
}

void CConfiguredStrategy::OnPause()
{
	m_running = false;
	m_previous = 0;
	Persist(m_task);
}
void CConfiguredStrategy::OnStop()
{
	OnPause();
}

void CConfiguredStrategy::OnMarketState(bool available)
{
	m_previous = 0;
	m_quoteTime = 0;
	if (available && (strategy::Kind::DualMaCross == m_kind))
	{
		m_historyReady = false;
		m_lastHistoryPoll = m_context->NowMs();
		m_context->RequestHistory(m_seedBegin);
	}
	if (!available && Protection() && !m_task && (0 < m_lastPosition) && ("已完成" != m_phase))
	{
		SetPhase("待处理", "行情中断，持仓周期和价格基准待确认");
	}
}

bool CConfiguredStrategy::Threshold(const strategy::CThreshold& threshold, std::int64_t price, const CPositionSnapshot& position, bool above) const
{
	if (!threshold.m_enabled)
	{
		return false;
	}
	if (!threshold.m_percent)
	{
		return above ? price >= threshold.m_value : price <= threshold.m_value;
	}
	if (!position.m_bCostKnown || (0 >= position.m_totalCost) || (0 >= position.m_totalQuantity))
	{
		return false;
	}
	std::int64_t current = 0, basis = 0;
	if (!utility::MultiplyDivide(price, position.m_totalQuantity, 1, current) || !utility::MultiplyDivide(position.m_totalCost, 10000, 1, basis))
	{
		return false;
	}
	std::int64_t factor = above ? 1000000 + threshold.m_value : 1000000 - threshold.m_value;
	if (0 > factor)
	{
		return false;
	}
	int comparison = CompareRatio(current, basis, factor, 1000000);
	return above ? 0 <= comparison : 0 >= comparison;
}

int CConfiguredStrategy::QuotePriority(const _TyQuoteData& quote) const
{
	if (strategy::Kind::TrailingTakeProfit == m_kind)
	{
		return 1;
	}
	if (strategy::Kind::TakeProfitStopLoss != m_kind)
	{
		return 3;
	}
	if ((0 > quote.price_scale()) || (6 < quote.price_scale()) || (0 >= quote.last_price()))
	{
		return 2;
	}
	std::int64_t price = 0;
	if (!utility::MultiplyDivide(quote.last_price(), Power(6 - quote.price_scale()), 1, price))
	{
		return 2;
	}
	return Threshold(m_settings.m_stopLoss, price, m_context->GetTradingSnapshot(m_security).m_position, false) ? 0 : 2;
}

std::int64_t CConfiguredStrategy::SellTarget(const CPositionSnapshot& position) const
{
	std::int64_t target = position.m_totalQuantity;
	if ("quantity" == m_settings.m_sellMode)
	{
		target = std::min(target, m_settings.m_sellValue);
	}
	if ("position_ratio" == m_settings.m_sellMode)
	{
		if (!utility::MultiplyDivide(target, m_settings.m_sellValue, 1000000, target))
		{
			return 0;
		}
	}
	return target;
}

std::int64_t CConfiguredStrategy::BuyQuantity(const CStrategyAccountView& view, const trade::CExecutionRule& rules) const
{
	const trade::CRule& rule = rules.m_rule;
	std::int64_t available = m_context->AvailableFunds(view);
	std::int64_t price = m_price / Power(6 - rule.m_nPriceScale);
	if ("quantity" == m_settings.m_buyMode)
	{
		return Rounded(m_settings.m_buyValue, rule.m_nBuyMinimum, rule.m_nBuyStep);
	}
	std::int64_t budget = std::min(available, m_settings.m_buyValue);
	if ("funds_ratio" == m_settings.m_buyMode)
	{
		if (!utility::MultiplyDivide(available, m_settings.m_buyValue, 1000000, budget))
		{
			return 0;
		}
	}
	std::int64_t maximum = 0;
	if (!utility::MultiplyDivide(budget, Power(rule.m_nPriceScale), price, maximum))
	{
		return 0;
	}
	maximum /= 100;
	std::int64_t minimum = 0;
	std::int64_t upper = maximum / rule.m_nBuyStep;
	while (minimum < upper)
	{
		std::int64_t middle = minimum + (upper - minimum + 1) / 2;
		std::int64_t debit = 0;
		if (trade::MaximumDebit(rule, price, middle * rule.m_nBuyStep, debit) && (debit <= budget))
		{
			minimum = middle;
		}
		else
		{
			upper = middle - 1;
		}
	}
	return Rounded(minimum * rule.m_nBuyStep, rule.m_nBuyMinimum, rule.m_nBuyStep);
}

void CConfiguredStrategy::Submit(bool buy, const CStrategyAccountView& view, const trade::CExecutionRule& rules, std::int64_t quantity)
{
	const trade::CRule& rule = rules.m_rule;
	std::int64_t price = m_price / Power(6 - rule.m_nPriceScale);
	if (buy)
	{
		quantity = BuyQuantity(view, rules);
	}
	else
	{
		quantity = std::min(quantity, view.m_position.m_availableQuantity);
		if (quantity != view.m_position.m_totalQuantity)
		{
			quantity = Rounded(quantity, rule.m_nSellMinimum, rule.m_nSellStep);
		}
	}
	if (0 >= quantity)
	{
		SetPhase(m_task ? "等待可卖" : "监控", "数量不足或无可用资金、可卖持仓");
		return;
	}
	std::int64_t debit = 0;
	if (buy && (!trade::MaximumDebit(rule, price, quantity, debit) || (debit > m_context->AvailableFunds(view))))
	{
		SetPhase(m_task ? "待处理" : "监控", "资金不足以覆盖本金和费用");
		return;
	}
	m_submittingSignal = m_context->AllocateSignal();
	if (0 == m_submittingSignal)
	{
		throw std::runtime_error("Cannot allocate a durable signal ID");
	}
	Persist(m_task);
	COrderIntent intent;
	intent.m_id = m_config.m_id;
	intent.m_signalId = m_submittingSignal;
	intent.m_security = m_security;
	intent.m_side = buy ? OrderSide::Buy : OrderSide::Sell;
	intent.m_positionEffect = buy ? PositionEffect::Open : PositionEffect::Close;
	intent.m_price = price;
	intent.m_nPriceScale = rule.m_nPriceScale;
	intent.m_quantity = quantity;
	intent.m_maximumDebit = debit;
	intent.m_quoteTimeMs = m_quoteTime;
	intent.m_basisVersion = m_confirmedBasis;
	COrderSubmitResult result = m_context->SubmitOrder(intent);
	m_pending = result.m_clientOrderId;
	m_submittingSignal = 0;
	Persist(m_task);
	if (0 != m_pending)
	{
		SetPhase(result.m_bAccepted ? "执行中" : "结果待核实", result.m_strReason);
	}
	else
	{
		SetPhase(result.m_bRetryable ? (m_task ? "等待可卖" : "监控") : "待处理", result.m_strReason);
	}
}

void CConfiguredStrategy::StartExit(const std::string& reason)
{
	m_task = true;
	m_target = -1;
	m_done = 0;
	m_taskPaused = false;
	m_reason = reason;
	m_phase = "等待委托终结";
	Persist(true);
	Tick();
}

void CConfiguredStrategy::Tick()
{
	if (!m_running || ("待处理" == m_phase) || (0 != m_submittingSignal))
	{
		return;
	}
	CStrategyAccountView view = m_context->GetTradingSnapshot(m_security);
	if (!view.m_account.m_bReady || !view.m_position.m_bReady)
	{
		SetPhase("等待账户同步", "资金或持仓尚未就绪");
		return;
	}
	if (0 != m_pending)
	{
		std::optional<CExecutionRecord> record = m_context->FindExecution(m_pending);
		if (!record.has_value() || (ExecutionState::Unknown == record->m_state))
		{
			SetPhase("结果待核实", "委托结果未知，请对账");
			return;
		}
		if (ExecutionState::NotSent == record->m_state)
		{
			SetPhase("待处理", record->m_message);
			return;
		}
		if (ExecutionState::Closed != record->m_state)
		{
			SetPhase("执行中", record->m_message);
			return;
		}
		if (trade::Status::Filled != record->m_brokerStatus)
		{
			SetPhase("待处理", record->m_message);
			return;
		}
		m_done += record->m_filledQuantity;
		m_pending = 0;
		Persist(m_task);
		if (!m_task)
		{
			SetPhase(strategy::Kind::DualMaCross == m_kind ? "监控" : "已完成");
		}
	}
	if (!m_task)
	{
		if (Protection() && ("已完成" == m_phase) && m_settings.m_autoRearm && m_seenZero && m_context->ReleaseExit(true))
		{
			SetPhase("等待持仓");
		}
		return;
	}
	if (m_taskPaused)
	{
		SetPhase("退出任务已暂停");
		return;
	}
	if (!m_context->ClaimExit())
	{
		SetPhase("等待其他保护任务");
		return;
	}
	bool active = false;
	for (const auto& order : view.m_orders)
	{
		if ((m_security.String() == order.m_strSecurity) && ((trade::Status::Accepted == order.m_status) || (trade::Status::Pending == order.m_status) || (trade::Status::PartiallyFilled == order.m_status) || (trade::Status::Unknown == order.m_status)))
		{
			active = true;
		}
	}
	if (active)
	{
		m_context->CancelConflicts();
		SetPhase("等待委托终结", "等待撤单结果及账户同步");
		return;
	}
	if (0 > m_target)
	{
		m_target = SellTarget(view.m_position);
		Persist(true);
	}
	std::int64_t remaining = std::min(m_target - m_done, view.m_position.m_totalQuantity);
	if (0 >= remaining)
	{
		if (!m_context->ReleaseExit(false))
		{
			SetPhase("等待账户同步");
			return;
		}
		m_task = false;
		SetPhase("已完成");
		Persist();
		return;
	}
	std::optional<trade::CExecutionRule> rules = m_context->GetRules(m_security);
	if (!rules.has_value() || (0 == m_quoteTime) || (5000 < m_context->NowMs() - m_quoteTime))
	{
		SetPhase("等待有效行情或交易规则");
		return;
	}
	Submit(false, view, *rules, remaining);
}

void CConfiguredStrategy::OnQuote(const _TyQuoteData& quote)
{
	if (!m_running || quote.stale() || (m_security.m_strCode != quote.security().symbol()) || (static_cast<int>(m_security.m_market) != static_cast<int>(quote.security().exchange())) || (0 >= quote.last_price()) || (0 > quote.price_scale()) || (6 < quote.price_scale()))
	{
		return;
	}
	std::int64_t time = quote.receive_time_ms();
	std::int64_t now = m_context->NowMs();
	if ((0 >= time) || (time <= m_quoteTime) || (5000 < now - time) || (1000 < time - now))
	{
		return;
	}
	std::int64_t price = 0;
	if (!utility::MultiplyDivide(quote.last_price(), Power(6 - quote.price_scale()), 1, price))
	{
		return;
	}
	if ((strategy::Kind::DualMaCross == m_kind) && (now - m_lastHistoryPoll >= 60000))
	{
		m_lastHistoryPoll = now;
		m_context->RequestHistory(m_historyReady ? m_lastBar : m_seedBegin);
	}
	m_price = price;
	m_quoteTime = time;
	m_rawPrice = quote.last_price();
	m_scale = quote.price_scale();
	if (m_task || (0 != m_pending))
	{
		Tick();
		if (m_task || (0 != m_pending))
		{
			return;
		}
	}
	if (("待处理" == m_phase) || ("结果待核实" == m_phase))
	{
		return;
	}
	CStrategyAccountView view = m_context->GetTradingSnapshot(m_security);
	if (!view.m_position.m_bReady || !view.m_account.m_bReady)
	{
		m_previous = 0;
		SetPhase("等待账户同步");
		return;
	}
	std::optional<trade::CExecutionRule> rules = m_context->GetRules(m_security);
	if (!rules.has_value())
	{
		m_previous = 0;
		SetPhase("等待交易规则", "柜台尚未提供可靠申报规则和费用上界");
		return;
	}
	const trade::CRule& rule = rules->m_rule;
	const trade::CCalendar& calendar = rules->m_calendar;
	if ((0 > rule.m_nPriceScale) || (6 < rule.m_nPriceScale) || (0 >= rule.m_nTick) || (0 >= rule.m_nBuyStep) || (0 >= rule.m_nSellStep) || (now < calendar.m_nOpen) || (now >= calendar.m_nClose) || ((now >= calendar.m_nBreak) && (now < calendar.m_nResume)))
	{
		SetPhase("等待交易时段");
		return;
	}
	std::int64_t factor = Power(6 - rule.m_nPriceScale);
	if ((0 != price % factor) || (0 != (price / factor) % rule.m_nTick) || (price / factor < rule.m_nLower) || (price / factor > rule.m_nUpper))
	{
		SetPhase("等待有效价格");
		return;
	}
	if (rule.m_bCorporateAction && (m_confirmedBasis != rule.m_strVersion))
	{
		SetPhase("待处理", "公司行动影响价格基准，请确认基准");
		return;
	}
	if (Protection())
	{
		if ((0 != m_epoch) && (m_epoch != view.m_connectionEpoch) && ("已完成" != m_phase))
		{
			m_epoch = view.m_connectionEpoch;
			SetPhase("待处理", "账户连接已变化，请确认持仓周期");
			return;
		}
		m_epoch = view.m_connectionEpoch;
		if (m_lastPosition != view.m_position.m_totalQuantity)
		{
			if ((0 == m_lastPosition) && (0 < view.m_position.m_totalQuantity) && ("已完成" != m_phase))
			{
				m_seenZero = false;
				m_tracking = false;
				m_highest = 0;
			}
			m_lastPosition = view.m_position.m_totalQuantity;
			if (0 == m_lastPosition)
			{
				m_seenZero = true;
				m_tracking = false;
				m_highest = 0;
			}
			Persist();
		}
		if ("已完成" == m_phase)
		{
			if (m_settings.m_autoRearm && m_seenZero && m_context->ReleaseExit(true))
			{
				SetPhase("等待持仓");
			}
			return;
		}
		if (0 >= view.m_position.m_totalQuantity)
		{
			SetPhase("等待持仓");
			return;
		}
		if (strategy::Kind::TakeProfitStopLoss == m_kind)
		{
			if (m_settings.m_takeProfit.m_enabled && m_settings.m_stopLoss.m_enabled && view.m_position.m_bCostKnown && ((m_settings.m_takeProfit.m_percent && !m_settings.m_stopLoss.m_percent && Threshold(m_settings.m_takeProfit, m_settings.m_stopLoss.m_value, view.m_position, true)) || (!m_settings.m_takeProfit.m_percent && m_settings.m_stopLoss.m_percent && Threshold(m_settings.m_stopLoss, m_settings.m_takeProfit.m_value, view.m_position, false))))
			{
				SetPhase("待处理", "当前成本下止损线不得高于或等于止盈线");
				return;
			}
			if (Threshold(m_settings.m_stopLoss, price, view.m_position, false))
			{
				StartExit("止损触发");
				return;
			}
			if (Threshold(m_settings.m_takeProfit, price, view.m_position, true))
			{
				StartExit("止盈触发");
				return;
			}
			if (((m_settings.m_takeProfit.m_enabled && m_settings.m_takeProfit.m_percent) || (m_settings.m_stopLoss.m_enabled && m_settings.m_stopLoss.m_percent)) && !view.m_position.m_bCostKnown)
			{
				SetPhase("等待有效成本");
				return;
			}
		}
		else
		{
			if (!m_tracking && Threshold(m_settings.m_activation, price, view.m_position, true))
			{
				m_tracking = true;
				m_highest = price;
				Persist();
			}
			if (m_tracking)
			{
				if (price > m_highest)
				{
					m_highest = price;
					Persist();
				}
				bool crossed = m_settings.m_drawdown.m_percent ? 0 >= CompareRatio(price, m_highest, 1000000 - m_settings.m_drawdown.m_value, 1000000) : ((m_highest >= m_settings.m_drawdown.m_value) && (price <= m_highest - m_settings.m_drawdown.m_value));
				if (crossed)
				{
					StartExit("移动止盈触发");
					return;
				}
			}
		}
		SetPhase(m_tracking ? "跟踪最高价" : "监控");
	}
	else if (strategy::Kind::PriceBreakout == m_kind)
	{
		if ("已完成" == m_phase)
		{
			if (!m_settings.m_autoRearm)
			{
				return;
			}
			if (m_settings.m_up ? price < m_settings.m_triggerPrice : price > m_settings.m_triggerPrice)
			{
				m_previous = price;
				SetPhase("监控");
			}
			return;
		}
		bool crossed = (0 != m_previous) && (m_settings.m_up ? ((m_previous < m_settings.m_triggerPrice) && (price >= m_settings.m_triggerPrice)) : ((m_previous > m_settings.m_triggerPrice) && (price <= m_settings.m_triggerPrice)));
		m_previous = price;
		if (crossed)
		{
			Submit(m_settings.m_buy, view, *rules, SellTarget(view.m_position));
		}
	}
}

void CConfiguredStrategy::OnBar(const _TyBarData& bar)
{
	if ((strategy::Kind::DualMaCross == m_kind) && m_historyReady && !m_feedingHistory && (bar.begin_time_ms() == m_lastBar) && (bar.data_version() > m_barVersion))
	{
		m_historyReady = false;
		m_context->RequestHistory(m_seedBegin);
		return;
	}
	if (!m_running || (("待处理" == m_phase) && !m_feedingHistory) || (strategy::Kind::DualMaCross != m_kind) || (!m_historyReady && !m_feedingHistory) || !bar.completed() || (0 == bar.data_version()) || ("none" != bar.adjustment()) || (bar.security().symbol() != m_security.m_strCode) || (static_cast<int>(bar.security().exchange()) != static_cast<int>(m_security.m_market)) || (static_cast<int>(bar.channel()) != static_cast<int>("1m" == m_settings.m_barPeriod ? Channel::bar_1m : Channel::bar_1d)) || (bar.begin_time_ms() <= m_lastBar) || (bar.end_time_ms() <= bar.begin_time_ms()) || (bar.end_time_ms() > m_context->NowMs()) || (0 >= bar.close_price()) || (0 > bar.price_scale()) || (6 < bar.price_scale()))
	{
		return;
	}
	if (bar.basis_version().empty())
	{
		SetPhase("待处理", "K 线价格基准版本缺失");
		return;
	}
	if (!m_barBasis.empty() && (m_barBasis != bar.basis_version()))
	{
		SetPhase("待处理", "K 线价格基准变化，请确认基准后重新预热");
		return;
	}
	m_barBasis = bar.basis_version();
	double price = static_cast<double>(bar.close_price()) / static_cast<double>(Power(bar.price_scale()));
	if (0 == m_seedBegin)
	{
		m_seedBegin = bar.begin_time_ms();
	}
	m_lastBar = bar.begin_time_ms();
	m_barVersion = bar.data_version();
	++m_samples;
	m_prices.emplace_back(price);
	if (m_prices.size() > static_cast<std::size_t>(m_settings.m_longWindow))
	{
		m_prices.erase(m_prices.begin());
	}
	if (m_samples == static_cast<std::uint64_t>(m_settings.m_shortWindow))
	{
		m_shortEma = std::accumulate(m_prices.begin(), m_prices.end(), 0.0) / m_settings.m_shortWindow;
	}
	else if (m_samples > static_cast<std::uint64_t>(m_settings.m_shortWindow))
	{
		m_shortEma += 2.0 / (m_settings.m_shortWindow + 1) * (price - m_shortEma);
	}
	if (m_samples == static_cast<std::uint64_t>(m_settings.m_longWindow))
	{
		m_longEma = std::accumulate(m_prices.begin(), m_prices.end(), 0.0) / m_settings.m_longWindow;
	}
	else if (m_samples > static_cast<std::uint64_t>(m_settings.m_longWindow))
	{
		m_longEma += 2.0 / (m_settings.m_longWindow + 1) * (price - m_longEma);
	}
	if (m_prices.size() < static_cast<std::size_t>(m_settings.m_longWindow))
	{
		SetPhase("预热", "完成的历史 K 线不足");
		Persist();
		return;
	}
	double shortAverage = m_shortEma, longAverage = m_longEma;
	if (!m_settings.m_ema)
	{
		std::string error;
		if ((nullptr == m_pIndicator) || !m_pIndicator->CalculateSma(m_prices, m_settings.m_shortWindow, shortAverage, error) || !m_pIndicator->CalculateSma(m_prices, m_settings.m_longWindow, longAverage, error))
		{
			SetPhase("待处理", error);
			return;
		}
	}
	double tolerance = 1e-12 * std::max({ 1.0, std::abs(shortAverage), std::abs(longAverage) });
	int relation = shortAverage > longAverage + tolerance ? 1 : (shortAverage < longAverage - tolerance ? -1 : 0);
	bool crossed = (0 != relation) && (0 != m_relation) && (relation != m_relation);
	if (0 != relation)
	{
		m_relation = relation;
	}
	Persist();
	if (m_feedingHistory)
	{
		return;
	}
	Tick();
	if (!crossed || (0 != m_pending) || ("待处理" == m_phase) || (0 == m_quoteTime) || (5000 < m_context->NowMs() - m_quoteTime))
	{
		return;
	}
	CStrategyAccountView view = m_context->GetTradingSnapshot(m_security);
	std::optional<trade::CExecutionRule> rules = m_context->GetRules(m_security);
	if (rules.has_value() && view.m_account.m_bReady)
	{
		Submit(0 < relation, view, *rules, SellTarget(view.m_position));
	}
}

void CConfiguredStrategy::OnHistory(const std::vector<_TyBarData>& data)
{
	if (strategy::Kind::DualMaCross != m_kind)
	{
		return;
	}
	if (m_historyReady)
	{
		std::size_t fresh = 0;
		std::int64_t latest = 0;
		for (const auto& bar : data)
		{
			if (bar.completed() && (bar.begin_time_ms() > m_lastBar))
			{
				++fresh;
				latest = std::max(latest, bar.end_time_ms());
			}
		}
		m_feedingHistory = (1 < fresh) || ((0 < fresh) && (5000 < m_context->NowMs() - latest));
		for (const auto& bar : data)
		{
			OnBar(bar);
		}
		m_feedingHistory = false;
		Persist();
		return;
	}
	if (!m_barBasis.empty() && (0 < m_seedBegin) && !std::any_of(data.begin(), data.end(), [this](const _TyBarData& bar)
	{
		return bar.completed() && (bar.begin_time_ms() == m_seedBegin) && (bar.basis_version() == m_barBasis);
	}))
	{
		SetPhase("待处理", "历史预热起点缺失，不能改变 EMA 计算起点");
		return;
	}
	bool attention = ("待处理" == m_phase) || ("结果待核实" == m_phase);
	std::string phase = m_phase, reason = m_reason;
	m_prices.clear();
	m_samples = 0;
	m_relation = 0;
	m_lastBar = 0;
	m_shortEma = 0;
	m_longEma = 0;
	m_feedingHistory = true;
	for (const auto& bar : data)
	{
		OnBar(bar);
	}
	m_feedingHistory = false;
	m_historyReady = true;
	if (attention)
	{
		m_phase = phase;
		m_reason = reason;
	}
	if ("待处理" != m_phase)
	{
		SetPhase(m_prices.size() < static_cast<std::size_t>(m_settings.m_longWindow) ? "预热" : "监控");
	}
	Persist();
}

void CConfiguredStrategy::OnOrder(const COrderEvent&)
{
	Tick();
}
void CConfiguredStrategy::OnTrade(const CTradeEvent&)
{
	Tick();
}
void CConfiguredStrategy::OnTimer(const CTimerEvent& event)
{
	if (event.m_zeroPosition && Protection())
	{
		m_seenZero = true;
		m_lastPosition = 0;
		m_tracking = false;
		m_highest = 0;
		Persist(m_task);
	}
	Tick();
}

bool CConfiguredStrategy::OnAction(const std::string& action)
{
	if ("pause_execution" == action)
	{
		if (!m_task)
		{
			return false;
		}
		m_taskPaused = true;
		Persist(m_task);
		SetPhase("退出任务已暂停");
		return true;
	}
	if ("resume_execution" == action)
	{
		if (!m_task)
		{
			return false;
		}
		if (0 != m_pending)
		{
			std::optional<CExecutionRecord> record = m_context->FindExecution(m_pending);
			if (!record.has_value() || ((ExecutionState::Closed != record->m_state) && (ExecutionState::NotSent != record->m_state)))
			{
				return false;
			}
			m_done += record->m_filledQuantity;
			m_pending = 0;
		}
		m_taskPaused = false;
		m_context->CancelConflicts(true);
		SetPhase("监控");
		Tick();
		return true;
	}
	if (("confirm_basis" == action) && m_task)
	{
		if ((0 != m_submittingSignal) || (0 != m_pending))
		{
			return false;
		}
		std::optional<trade::CExecutionRule> rules = m_context->GetRules(m_security);
		if (!rules.has_value())
		{
			return false;
		}
		m_confirmedBasis = rules->m_rule.m_strVersion;
		m_taskPaused = true;
		SetPhase("退出任务已暂停", "价格基准已确认，请核实剩余退出数量后恢复任务");
		Persist(true);
		return true;
	}
	if (("rearm" != action) && ("unlock" != action) && ("confirm_basis" != action))
	{
		return false;
	}
	if (m_task || (0 != m_submittingSignal))
	{
		return false;
	}
	if (0 != m_pending)
	{
		std::optional<CExecutionRecord> record = m_context->FindExecution(m_pending);
		if (!record.has_value() || ((ExecutionState::Closed != record->m_state) && (ExecutionState::NotSent != record->m_state)))
		{
			return false;
		}
		m_pending = 0;
	}
	if ("unlock" == action)
	{
		return m_context->ReleaseExit(true);
	}
	if ("confirm_basis" == action)
	{
		std::optional<trade::CExecutionRule> rules = m_context->GetRules(m_security);
		if (!rules.has_value())
		{
			return false;
		}
		m_confirmedBasis = rules->m_rule.m_strVersion;
		if (strategy::Kind::DualMaCross == m_kind)
		{
			m_seedBegin = rules->m_calendar.m_nOpen;
			m_barBasis.clear();
		}
	}
	if (!m_context->ReleaseExit(true))
	{
		return false;
	}
	m_previous = 0;
	m_tracking = false;
	m_highest = 0;
	m_seenZero = false;
	m_lastPosition = 0;
	m_target = -1;
	m_done = 0;
	SetPhase("监控");
	Persist();
	if (strategy::Kind::DualMaCross == m_kind)
	{
		m_historyReady = false;
		m_lastHistoryPoll = m_context->NowMs();
		m_context->RequestHistory(m_seedBegin);
	}
	return true;
}
