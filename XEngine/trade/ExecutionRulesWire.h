#pragma once
#include "TradeTypes.h"
#include "../request/request.pb.h"

namespace trade
{
	inline void WriteExecutionRules(const CExecutionRule& source, request::ExecutionRules& target)
	{
		target.set_security(source.m_rule.m_security);
		target.set_version(source.m_rule.m_version_text);
		target.set_trading_day(source.m_rule.m_day);
		target.set_price_scale(source.m_rule.m_price_scale);
		target.set_tick(source.m_rule.m_tick);
		target.set_buy_minimum(source.m_rule.m_buy_minimum);
		target.set_buy_step(source.m_rule.m_buy_step);
		target.set_sell_minimum(source.m_rule.m_sell_minimum);
		target.set_sell_step(source.m_rule.m_sell_step);
		target.set_lower(source.m_rule.m_lower);
		target.set_upper(source.m_rule.m_upper);
		target.set_commission_ppm(source.m_rule.m_commission_ppm);
		target.set_minimum_fee_cents(source.m_rule.m_minimum_fee);
		target.set_sell_tax_ppm(source.m_rule.m_sell_tax_ppm);
		target.set_transfer_ppm(source.m_rule.m_transfer_ppm);
		target.set_corporate_action(source.m_rule.m_corporate_action);
		target.set_open_time_ms(source.m_calendar.m_open_time_ms);
		target.set_break_time_ms(source.m_calendar.m_break_time_ms);
		target.set_resume_time_ms(source.m_calendar.m_resume_time_ms);
		target.set_close_time_ms(source.m_calendar.m_close_time_ms);
		target.set_reliable_query(source.m_reliable_query);
		target.set_idempotent_client_id(source.m_idempotent_client_id);
	}

	inline CExecutionRule ReadExecutionRules(const request::ExecutionRules& source)
	{
		CExecutionRule target;
		target.m_rule.m_security = source.security();
		target.m_rule.m_version_text = source.version();
		target.m_rule.m_day = source.trading_day();
		target.m_rule.m_price_scale = source.price_scale();
		target.m_rule.m_tick = source.tick();
		target.m_rule.m_buy_minimum = source.buy_minimum();
		target.m_rule.m_buy_step = source.buy_step();
		target.m_rule.m_sell_minimum = source.sell_minimum();
		target.m_rule.m_sell_step = source.sell_step();
		target.m_rule.m_lower = source.lower();
		target.m_rule.m_upper = source.upper();
		target.m_rule.m_commission_ppm = source.commission_ppm();
		target.m_rule.m_minimum_fee = source.minimum_fee_cents();
		target.m_rule.m_sell_tax_ppm = source.sell_tax_ppm();
		target.m_rule.m_transfer_ppm = source.transfer_ppm();
		target.m_rule.m_corporate_action = source.corporate_action();
		target.m_calendar.m_open_time_ms = source.open_time_ms();
		target.m_calendar.m_break_time_ms = source.break_time_ms();
		target.m_calendar.m_resume_time_ms = source.resume_time_ms();
		target.m_calendar.m_close_time_ms = source.close_time_ms();
		target.m_calendar.m_day = source.trading_day();
		target.m_reliable_query = source.reliable_query();
		target.m_idempotent_client_id = source.idempotent_client_id();
		return target;
	}
} // namespace trade
