#pragma once

#include "../IStrategy.h"
#include "../StrategySettings.h"
#include "../../trade/StrategyAccountView.h"

class CIndicator;

class CConfiguredStrategy : public IStrategy
{
public:
	explicit CConfiguredStrategy(strategy::Kind kind);
	static bool ValidateParameters(const CStrategyConfig& config, std::string& error);
	bool Initialize(IStrategyContext& context, const CStrategyConfig& config) override;
	void OnStart() override;
	void OnPause() override;
	void OnStop() override;
	void OnQuote(const _TyQuoteData& data) override;
	void OnBar(const _TyBarData& data) override;
	void OnOrder(const COrderEvent&) override;
	void OnTrade(const CTradeEvent&) override;
	void OnTimer(const CTimerEvent&) override;
	void OnHistory(const std::vector<_TyBarData>& data) override;
	void OnMarketState(bool available) override;
	bool OnAction(const std::string& action) override;
	int QuotePriority(const _TyQuoteData& data) const override;

protected:
	CIndicator* m_pIndicator{ nullptr };

private:
	bool Protection() const;
	bool Threshold(const strategy::CThreshold& threshold, std::int64_t price, const CPositionSnapshot& position, bool above) const;
	void SetPhase(const std::string& phase, const std::string& reason = {});
	void Persist(bool protect = false);
	void Restore(const std::string& data);
	void Tick();
	void Submit(bool buy, const CStrategyAccountView& view, const trade::CExecutionRule& rule, std::int64_t quantity);
	void StartExit(const std::string& reason);
	std::int64_t SellTarget(const CPositionSnapshot& position) const;
	std::int64_t BuyQuantity(const CStrategyAccountView& view, const trade::CExecutionRule& rules) const;
	std::string m_signature;
	strategy::Kind m_kind;
	strategy::CSettings m_settings;
	IStrategyContext* m_context{ nullptr };
	CStrategyConfig m_config;
	CSecurity m_security;
	std::string m_phase{ "监控" };
	std::string m_reason;
	bool m_running{ false };
	bool m_taskPaused{ false };
	bool m_task{ false };
	bool m_historyReady{ false };
	bool m_feedingHistory{ false };
	bool m_tracking{ false };
	bool m_seenZero{ false };
	std::int64_t m_highest{ 0 };
	std::int64_t m_previous{ 0 };
	std::int64_t m_quoteTime{ 0 };
	std::int64_t m_lastHistoryPoll{ 0 };
	std::int64_t m_price{ 0 };
	int m_scale{ 2 };
	std::int64_t m_rawPrice{ 0 };
	std::int64_t m_lastPosition{ 0 };
	std::uint64_t m_epoch{ 0 };
	std::int64_t m_target{ -1 };
	std::int64_t m_done{ 0 };
	std::uint64_t m_pending{ 0 };
	std::uint64_t m_submittingSignal{ 0 };
	std::string m_confirmedBasis;
	std::string m_barBasis;
	std::int64_t m_lastBar{ 0 };
	std::int64_t m_seedBegin{ 0 };
	std::uint64_t m_barVersion{ 0 };
	std::uint64_t m_samples{ 0 };
	int m_relation{ 0 };
	double m_shortEma{ 0 };
	double m_longEma{ 0 };
	std::vector<double> m_prices;
};
