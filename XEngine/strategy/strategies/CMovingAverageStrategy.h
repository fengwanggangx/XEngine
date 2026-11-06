#ifndef XENGINE_STRATEGY_STRATEGIES_CMOVINGAVERAGESTRATEGY_H
#define XENGINE_STRATEGY_STRATEGIES_CMOVINGAVERAGESTRATEGY_H

#include "../IStrategy.h"

#include <cstdint>
#include <deque>

class CMovingAverageStrategy final : public IStrategy
{
	public:
	bool Initialize(IStrategyContext& context, const CStrategyConfig& config) override;
	void OnStart() override;
	void OnPause() override;
	void OnStop() override;
	void OnQuote(const _TyQuoteData& data) override;
	void OnOrder(const COrderEvent& event) override;
	void OnTrade(const CTradeEvent& event) override;

	private:
	void UpdatePrices(std::int64_t price);
	void TryGenerateSignal(const _TyQuoteData& data);
	static bool ReadWindow(const CStrategyConfig& config, const std::string& strName, std::size_t& window);

	private:
	IStrategyContext* m_pContext{ nullptr };
	_TyStrategyId m_id{ 0 };
	std::deque<std::int64_t> m_prices;
	std::size_t m_shortWindow{ 5 };
	std::size_t m_longWindow{ 20 };
	_TySignalId m_nextSignalId{ 1 };
	int m_lastRelation{ 0 };
	bool m_bRunning{ false };
};

#endif
