#include "CDualMaCrossStrategy.h"

CDualMaCrossStrategy::CDualMaCrossStrategy(CIndicator& indicator) : CConfiguredStrategy(strategy::Kind::DualMaCross)
{
	m_indicator = &indicator;
}
