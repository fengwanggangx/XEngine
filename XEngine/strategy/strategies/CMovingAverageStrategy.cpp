#include "CMovingAverageStrategy.h"

#include "../IStrategyContext.h"
#include "../../indicator/CIndicator.h"

#include <algorithm>
#include <cmath>

#include <charconv>
#include <string>

CMovingAverageStrategy::CMovingAverageStrategy(CIndicator& indicator) : m_indicator(indicator)
{
}

bool CMovingAverageStrategy::Initialize(IStrategyContext& context, const CStrategyConfig& config)
{
	std::size_t shortWindow = 5;
	std::size_t longWindow = 20;
	if (!ReadWindows(config, shortWindow, longWindow))
	{
		return false;
	}
	m_states.clear();
	for (const auto& subscription : config.m_subscriptions)
	{
		if ((Channel::quote == subscription.m_channel) && subscription.m_security.IsValid())
		{
			m_states.try_emplace({ subscription.m_security.m_market, subscription.m_security.m_strCode });
		}
	}
	if (m_states.empty())
	{
		context.WriteLog(LogLevel::Error, "Moving-average strategy requires a quote subscription");
		return false;
	}
	m_bRunning = false;
	m_pContext = &context;
	m_id = config.m_id;
	m_shortWindow = shortWindow;
	m_longWindow = longWindow;
	return true;
}

void CMovingAverageStrategy::OnStart()
{
	m_bRunning = true;
}

void CMovingAverageStrategy::OnPause()
{
	m_bRunning = false;
}

void CMovingAverageStrategy::OnStop()
{
	m_bRunning = false;
	for (auto& [key, state] : m_states)
	{
		state.m_prices.clear();
		state.m_input.clear();
		state.m_nextPrice = 0;
		state.m_lastRelation = 0;
		state.m_bCalculationFailed = false;
	}
}

void CMovingAverageStrategy::OnQuote(const _TyQuoteData& data)
{
	if (!m_bRunning || (0 >= data.last_price()))
	{
		return;
	}
	if ((0 > data.price_scale()) || (18 < data.price_scale()))
	{
		return;
	}
	const auto mIter = m_states.find({ static_cast<Exchange>(static_cast<int>(data.security().exchange())), data.security().symbol() });
	if (m_states.end() == mIter)
	{
		return;
	}
	double price = static_cast<double>(data.last_price()) / std::pow(10.0, data.price_scale());
	if (!std::isfinite(price) || (0.0 >= price))
	{
		return;
	}
	CSecurityState& state = mIter->second;
	if (state.m_prices.empty())
	{
		state.m_prices.reserve(m_longWindow);
		state.m_input.reserve(m_longWindow);
	}
	if (state.m_prices.size() < m_longWindow)
	{
		state.m_prices.emplace_back(price);
	}
	else
	{
		state.m_prices[state.m_nextPrice] = price;
		state.m_nextPrice = (state.m_nextPrice + 1) % m_longWindow;
	}
	if (state.m_prices.size() < m_longWindow)
	{
		return;
	}
	state.m_input.clear();
	state.m_input.insert(state.m_input.end(), state.m_prices.begin() + state.m_nextPrice, state.m_prices.end());
	state.m_input.insert(state.m_input.end(), state.m_prices.begin(), state.m_prices.begin() + state.m_nextPrice);
	TryGenerateSignal(data, state);
}

void CMovingAverageStrategy::OnOrder(const COrderEvent& event)
{
	if ((nullptr != m_pContext) && (OrderStatus::Rejected == event.m_status))
	{
		m_pContext->WriteLog(LogLevel::Warning, "Moving-average order was rejected: " + event.m_strReason);
	}
}

void CMovingAverageStrategy::OnTrade(const CTradeEvent& event)
{
	if (nullptr != m_pContext)
	{
		m_pContext->WriteLog(LogLevel::Info, "Moving-average order traded: " + event.m_strTradeId);
	}
}

void CMovingAverageStrategy::TryGenerateSignal(const _TyQuoteData& data, CSecurityState& state)
{
	if (nullptr == m_pContext)
	{
		return;
	}
	double shortAverage = 0.0;
	double longAverage = 0.0;
	std::string error;
	if (!m_indicator.CalculateSma(state.m_input, m_shortWindow, shortAverage, error) || !m_indicator.CalculateSma(state.m_input, m_longWindow, longAverage, error))
	{
		if (!state.m_bCalculationFailed)
		{
			m_pContext->WriteLog(LogLevel::Error, "Moving-average calculation failed for " + data.security().symbol() + ": " + error);
			state.m_bCalculationFailed = true;
		}
		return;
	}
	state.m_bCalculationFailed = false;
	double tolerance = 1e-12 * std::max({ 1.0, std::abs(shortAverage), std::abs(longAverage) });
	double difference = shortAverage - longAverage;
	int relation = difference > tolerance ? 1 : (difference < -tolerance ? -1 : 0);
	if ((0 == relation) || (0 == state.m_lastRelation) || (relation == state.m_lastRelation))
	{
		state.m_lastRelation = relation;
		return;
	}
	COrderIntent intent;
	intent.m_id = m_id;
	intent.m_signalId = m_nextSignalId++;
	intent.m_security = CSecurity(data.security().symbol(), static_cast<Exchange>(static_cast<int>(data.security().exchange())));
	intent.m_side = 0 < relation ? OrderSide::Buy : OrderSide::Sell;
	intent.m_orderType = OrderType::Limit;
	intent.m_positionEffect = 0 < relation ? PositionEffect::Open : PositionEffect::Close;
	intent.m_price = data.last_price();
	intent.m_nPriceScale = data.price_scale();
	intent.m_quantity = 100;
	intent.m_strReason = 0 < relation ? "Short moving average crossed above long moving average" : "Short moving average crossed below long moving average";
	COrderSubmitResult result = m_pContext->SubmitOrder(intent);
	if (!result.m_bAccepted)
	{
		m_pContext->WriteLog(LogLevel::Warning, "Moving-average signal was rejected: " + result.m_strReason);
	}
	state.m_lastRelation = relation;
}

bool CMovingAverageStrategy::ValidateParameters(const CStrategyConfig& config)
{
	std::size_t shortWindow = 5;
	std::size_t longWindow = 20;
	return ReadWindows(config, shortWindow, longWindow) && std::any_of(config.m_subscriptions.begin(), config.m_subscriptions.end(), [](const CQuoteInfo& subscription)
	{
		return (Channel::quote == subscription.m_channel) && subscription.m_security.IsValid();
	});
}

bool CMovingAverageStrategy::ReadWindows(const CStrategyConfig& config, std::size_t& shortWindow, std::size_t& longWindow)
{
	return ReadWindow(config, "short_window", shortWindow) && ReadWindow(config, "long_window", longWindow) && (shortWindow < longWindow);
}

bool CMovingAverageStrategy::ReadWindow(const CStrategyConfig& config, const std::string& strName, std::size_t& window)
{
	const auto mIter = config.m_params.find(strName);
	if (config.m_params.end() == mIter)
	{
		return true;
	}
	std::size_t parsedWindow = 0;
	const char* pBegin = mIter->second.data();
	const char* pEnd = pBegin + mIter->second.size();
	std::from_chars_result result = std::from_chars(pBegin, pEnd, parsedWindow);
	if ((std::errc{} != result.ec) || (pEnd != result.ptr) || (0 == parsedWindow) || (100000 < parsedWindow))
	{
		return false;
	}
	window = parsedWindow;
	return true;
}
