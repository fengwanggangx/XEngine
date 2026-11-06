#include "CMovingAverageStrategy.h"

#include "../IStrategyContext.h"

#include <charconv>
#include <string>

bool CMovingAverageStrategy::Initialize(IStrategyContext& context, const CStrategyConfig& config)
{
	std::size_t shortWindow = m_shortWindow;
	std::size_t longWindow = m_longWindow;
	if (!ReadWindow(config, "short_window", shortWindow) || !ReadWindow(config, "long_window", longWindow) || (shortWindow >= longWindow))
	{
		return false;
	}
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
	m_prices.clear();
	m_lastRelation = 0;
}

void CMovingAverageStrategy::OnQuote(const _TyQuoteData& data)
{
	if (!m_bRunning || (0 >= data.last_price()))
	{
		return;
	}
	UpdatePrices(data.last_price());
	TryGenerateSignal(data);
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

void CMovingAverageStrategy::UpdatePrices(std::int64_t price)
{
	m_prices.emplace_back(price);
	if (m_longWindow < m_prices.size())
	{
		m_prices.pop_front();
	}
}

void CMovingAverageStrategy::TryGenerateSignal(const _TyQuoteData& data)
{
	if ((m_longWindow > m_prices.size()) || (nullptr == m_pContext))
	{
		return;
	}
	std::int64_t longSum = 0;
	std::int64_t shortSum = 0;
	for (std::size_t i = 0; i < m_prices.size(); ++i)
	{
		longSum += m_prices[i];
		if ((m_prices.size() - m_shortWindow) <= i)
		{
			shortSum += m_prices[i];
		}
	}
	std::int64_t scaledShort = shortSum * static_cast<std::int64_t>(m_longWindow);
	std::int64_t scaledLong = longSum * static_cast<std::int64_t>(m_shortWindow);
	int relation = scaledShort > scaledLong ? 1 : (scaledShort < scaledLong ? -1 : 0);
	if ((0 == relation) || (0 == m_lastRelation) || (relation == m_lastRelation))
	{
		m_lastRelation = relation;
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
	m_lastRelation = relation;
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
	if ((std::errc{ } != result.ec) || (pEnd != result.ptr) || (0 == parsedWindow))
	{
		return false;
	}
	window = parsedWindow;
	return true;
}
