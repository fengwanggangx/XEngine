#include "StrategyTypes.h"

#include <unordered_set>

bool CStrategyConfig::IsValid() const
{
	if (!IsAccountValid(m_account) || (0 == m_id) || m_str_type.empty() || m_str_name.empty() || (0 == m_event_queue_limit) || m_subscriptions.empty())
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
