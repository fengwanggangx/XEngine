#ifndef XENGINE_STRATEGY_STRATEGIES_CMOVINGAVERAGESTRATEGY_H
#define XENGINE_STRATEGY_STRATEGIES_CMOVINGAVERAGESTRATEGY_H

#include "../IStrategy.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class CIndicator;

class CMovingAverageStrategy final : public IStrategy
{
public:
	explicit CMovingAverageStrategy(CIndicator& indicator);
	static bool ValidateParameters(const CStrategyConfig& config);

	void OnStop() override;
	void OnStart() override;
	void OnPause() override;
	void OnQuote(const _TyQuoteData& data) override;
	void OnOrder(const COrderEvent& event) override;
	void OnTrade(const CTradeEvent& event) override;
	bool Initialize(IStrategyContext& context, const CStrategyConfig& config) override;

private:
	struct CSecurityState
	{
		std::vector<double> m_prices;
		std::vector<double> m_input;
		std::size_t m_nextPrice{ 0 };
		int m_lastRelation{ 0 };
		bool m_bCalculationFailed{ false };
	};
	using _TySecurityKey = std::pair<Exchange, std::string>;

	void TryGenerateSignal(const _TyQuoteData& data, CSecurityState& state);
	static bool ReadWindow(const CStrategyConfig& config, const std::string& strName, std::size_t& window);
	static bool ReadWindows(const CStrategyConfig& config, std::size_t& shortWindow, std::size_t& longWindow);

private:
	CIndicator& m_indicator;
	IStrategyContext* m_pContext{ nullptr };
	_TyStrategyId m_id{ 0 };
	std::map<_TySecurityKey, CSecurityState> m_states;
	std::size_t m_shortWindow{ 5 };
	std::size_t m_longWindow{ 20 };
	_TySignalId m_nextSignalId{ 1 };
	bool m_bRunning{ false };
};

#endif
