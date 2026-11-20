#pragma once
#include "TradeTypes.h"
#include "../common/numeric.h"
#include <algorithm>

namespace trade
{
	inline bool MaximumDebit(const CRule& rule, std::int64_t price, std::int64_t quantity, std::int64_t& debit)
	{
		std::int64_t principal = 0, commission = 0, transfer = 0, scale = 1;
		if ((0 > rule.m_price_scale) || (6 < rule.m_price_scale) || (0 > rule.m_commission_ppm) || (1000000 < rule.m_commission_ppm) || (0 > rule.m_transfer_ppm) || (1000000 < rule.m_transfer_ppm) || (0 > rule.m_minimum_fee))
		{
			return false;
		}
		for (int index = 0; rule.m_price_scale > index; ++index)
		{
			scale *= 10;
		}
		if (!utility::MultiplyDivide(price, quantity, 1, principal) || !utility::MultiplyDivide(principal, 100, scale, principal, true) || !utility::MultiplyDivide(principal, rule.m_commission_ppm, 1000000, commission, true) || !utility::MultiplyDivide(principal, rule.m_transfer_ppm, 1000000, transfer, true))
		{
			return false;
		}
		commission = std::max(commission, rule.m_minimum_fee);
		if ((principal > (std::numeric_limits<std::int64_t>::max)() - commission) || (principal + commission > (std::numeric_limits<std::int64_t>::max)() - transfer))
		{
			return false;
		}
		debit = principal + commission + transfer;
		return true;
	}
} // namespace trade
