#include "StrategyTypes.h"

#include <unordered_set>

bool CStrategyConfig::IsValid() const
{
	if (!IsAccountValid(m_accountIndex) || (0 == m_strategyId) || m_strType.empty() || m_strName.empty() || (0 == m_eventQueueLimit) || m_subscriptions.empty())
	{
		return false;
	}
	std::unordered_set<std::string> keys;
	keys.reserve(m_subscriptions.size());
	for (const auto& subscription : m_subscriptions)
	{
		if (!subscription.IsValid() || !keys.emplace(subscription.String()).second)
		{
			return false;
		}
	}
	return true;
}
