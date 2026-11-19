#include "TradingTypes.h"

bool COrderIntent::IsValid() const
{
	if ((0 == m_id) || (0 == m_signal_id) || !m_security.IsValid() || (OrderSide::Unknown == m_side) || (PositionEffect::Unknown == m_position_effect))
	{
		return false;
	}
	if ((0 >= m_quantity) || ((OrderType::Limit == m_order_type) && (0 >= m_price)))
	{
		return false;
	}
	return true;
}
