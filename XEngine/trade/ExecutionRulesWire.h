#pragma once
#include "TradeTypes.h"
#include "../request/request.pb.h"

namespace trade
{
	inline void WriteExecutionRules(const CExecutionRule& source, request::ExecutionRules& target)
	{
		target.set_security(source.m_rule.m_strSecurity);
		target.set_version(source.m_rule.m_strVersion);
		target.set_trading_day(source.m_rule.m_nDay);
		target.set_price_scale(source.m_rule.m_n_price_scale);
		target.set_tick(source.m_rule.m_nTick);
		target.set_buy_minimum(source.m_rule.m_n_buy_minimum);
		target.set_buy_step(source.m_rule.m_n_buy_step);
		target.set_sell_minimum(source.m_rule.m_n_sell_minimum);
		target.set_sell_step(source.m_rule.m_n_sell_step);
		target.set_lower(source.m_rule.m_nLower);
		target.set_upper(source.m_rule.m_nUpper);
		target.set_commission_ppm(source.m_rule.m_n_commission_ppm);
		target.set_minimum_fee_cents(source.m_rule.m_n_minimum_fee);
		target.set_sell_tax_ppm(source.m_rule.m_n_sell_tax_ppm);
		target.set_transfer_ppm(source.m_rule.m_n_transfer_ppm);
		target.set_corporate_action(source.m_rule.m_b_corporate_action);
		target.set_open_time_ms(source.m_calendar.m_nOpen);
		target.set_break_time_ms(source.m_calendar.m_nBreak);
		target.set_resume_time_ms(source.m_calendar.m_nResume);
		target.set_close_time_ms(source.m_calendar.m_nClose);
		target.set_reliable_query(source.m_reliable_query);
		target.set_idempotent_client_id(source.m_idempotent_client_id);
	}

	inline CExecutionRule ReadExecutionRules(const request::ExecutionRules& source)
	{
		CExecutionRule target;
		target.m_rule.m_strSecurity = source.security();
		target.m_rule.m_strVersion = source.version();
		target.m_rule.m_nDay = source.trading_day();
		target.m_rule.m_n_price_scale = source.price_scale();
		target.m_rule.m_nTick = source.tick();
		target.m_rule.m_n_buy_minimum = source.buy_minimum();
		target.m_rule.m_n_buy_step = source.buy_step();
		target.m_rule.m_n_sell_minimum = source.sell_minimum();
		target.m_rule.m_n_sell_step = source.sell_step();
		target.m_rule.m_nLower = source.lower();
		target.m_rule.m_nUpper = source.upper();
		target.m_rule.m_n_commission_ppm = source.commission_ppm();
		target.m_rule.m_n_minimum_fee = source.minimum_fee_cents();
		target.m_rule.m_n_sell_tax_ppm = source.sell_tax_ppm();
		target.m_rule.m_n_transfer_ppm = source.transfer_ppm();
		target.m_rule.m_b_corporate_action = source.corporate_action();
		target.m_calendar.m_nOpen = source.open_time_ms();
		target.m_calendar.m_nBreak = source.break_time_ms();
		target.m_calendar.m_nResume = source.resume_time_ms();
		target.m_calendar.m_nClose = source.close_time_ms();
		target.m_calendar.m_nDay = source.trading_day();
		target.m_reliable_query = source.reliable_query();
		target.m_idempotent_client_id = source.idempotent_client_id();
		return target;
	}
} // namespace trade
