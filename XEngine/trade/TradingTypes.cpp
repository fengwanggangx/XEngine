#include "TradingTypes.h"
bool COrderIntent::IsValid() const
{
	if ((0 == m_strategyId) || (0 == m_signalId) || !m_security.IsValid() || (OrderSide::Unknown == m_side) || (PositionEffect::Unknown == m_positionEffect))
	{
		return false;
	}
	if ((0 >= m_quantity) || ((OrderType::Limit == m_orderType) && (0 >= m_price)))
	{
		return false;
	}
	return true;
}
