#include "CSubscriptionMgr.h"

#include <charconv>
#include <mutex>
#include <string_view>

std::string FmtHQSubID(SubType type, std::uint64_t id)
{
	switch (type)
	{
	case SubType::Client:
		return "client_" + std::to_string(id);
	case SubType::Strategy:
		return "strategy_" + std::to_string(id);
	default:
		return {};
	}
}

bool ParseHQSubID(const std::string& text, SubType& type, std::uint64_t& id)
{
	std::string_view value(text);
	SubType parsedType = SubType::Unknown;
	if (value.starts_with("client_"))
	{
		parsedType = SubType::Client;
		value.remove_prefix(7);
	}
	else if (value.starts_with("strategy_"))
	{
		parsedType = SubType::Strategy;
		value.remove_prefix(9);
	}
	else
	{
		return false;
	}
	if (value.empty() || ((1 < value.size()) && ('0' == value.front())))
	{
		return false;
	}
	std::uint64_t parsedId = 0;
	std::from_chars_result result = std::from_chars(value.data(), value.data() + value.size(), parsedId);
	if ((std::errc{} != result.ec) || (value.data() + value.size() != result.ptr))
	{
		return false;
	}
	type = parsedType;
	id = parsedId;
	return true;
}

std::vector<CQuoteInfo> CSubscriptionMgr::Subscribe(const std::string& id, const std::vector<CQuoteInfo>& subscriptions)
{
	std::vector<CQuoteInfo> added;
	if (id.empty() || subscriptions.empty())
	{
		return added;
	}
	added.reserve(subscriptions.size());
	std::lock_guard<std::shared_mutex> lock(m_mtx_info);
	_TySubscriptions& infos = m_id_infos[id];
	for (const auto& info : subscriptions)
	{
		std::string strKey = info.String();
		if (!infos.emplace(strKey, info).second)
		{
			continue;
		}
		_TySubscriberIds& ids = m_info_ids[strKey];
		ids.emplace(id);
		if (1 == ids.size())
		{
			added.emplace_back(info);
		}
	}
	return added;
}

std::vector<CQuoteInfo> CSubscriptionMgr::Unsubscribe(const std::string& id, const std::vector<CQuoteInfo>& subscriptions)
{
	std::vector<CQuoteInfo> removed;
	removed.reserve(subscriptions.size());
	std::lock_guard<std::shared_mutex> lock(m_mtx_info);
	auto subscriberIter = m_id_infos.find(id);
	if (m_id_infos.end() == subscriberIter)
	{
		return removed;
	}
	for (const auto& info : subscriptions)
	{
		std::string strKey = info.String();
		auto infoIter = subscriberIter->second.find(strKey);
		if (subscriberIter->second.end() == infoIter)
		{
			continue;
		}
		auto idsIter = m_info_ids.find(strKey);
		if (m_info_ids.end() != idsIter)
		{
			idsIter->second.erase(id);
			if (idsIter->second.empty())
			{
				removed.emplace_back(infoIter->second);
				m_info_ids.erase(idsIter);
			}
		}
		subscriberIter->second.erase(infoIter);
	}
	if (subscriberIter->second.empty())
	{
		m_id_infos.erase(subscriberIter);
	}
	return removed;
}

std::vector<CQuoteInfo> CSubscriptionMgr::RemoveSubscriber(const std::string& id)
{
	std::vector<CQuoteInfo> removed;
	std::lock_guard<std::shared_mutex> lock(m_mtx_info);
	auto subscriberIter = m_id_infos.find(id);
	if (m_id_infos.end() == subscriberIter)
	{
		return removed;
	}
	removed.reserve(subscriberIter->second.size());
	for (const auto& info : subscriberIter->second)
	{
		auto idsIter = m_info_ids.find(info.first);
		if (m_info_ids.end() == idsIter)
		{
			continue;
		}
		idsIter->second.erase(id);
		if (idsIter->second.empty())
		{
			removed.emplace_back(info.second);
			m_info_ids.erase(idsIter);
		}
	}
	m_id_infos.erase(subscriberIter);
	return removed;
}

void CSubscriptionMgr::RemoveSubscription(const std::string& strKey)
{
	std::lock_guard<std::shared_mutex> lock(m_mtx_info);
	auto idsIter = m_info_ids.find(strKey);
	if (m_info_ids.end() == idsIter)
	{
		return;
	}
	for (const std::string& id : idsIter->second)
	{
		auto subscriberIter = m_id_infos.find(id);
		if (m_id_infos.end() == subscriberIter)
		{
			continue;
		}
		subscriberIter->second.erase(strKey);
		if (subscriberIter->second.empty())
		{
			m_id_infos.erase(subscriberIter);
		}
	}
	m_info_ids.erase(idsIter);
}

std::vector<std::string> CSubscriptionMgr::GetSubscriberIds(const std::string& strKey) const
{
	std::vector<std::string> ids;
	std::shared_lock<std::shared_mutex> lock(m_mtx_info);
	auto idsIter = m_info_ids.find(strKey);
	if (m_info_ids.end() != idsIter)
	{
		ids.assign(idsIter->second.begin(), idsIter->second.end());
	}
	return ids;
}

std::vector<CQuoteInfo> CSubscriptionMgr::GetSubscriptions() const
{
	std::vector<CQuoteInfo> subscriptions;
	std::shared_lock<std::shared_mutex> lock(m_mtx_info);
	subscriptions.reserve(m_info_ids.size());
	for (const auto& item : m_info_ids)
	{
		for (const std::string& id : item.second)
		{
			const auto subscriberIter = m_id_infos.find(id);
			if (m_id_infos.end() == subscriberIter)
			{
				continue;
			}
			const auto infoIter = subscriberIter->second.find(item.first);
			if (subscriberIter->second.end() != infoIter)
			{
				subscriptions.emplace_back(infoIter->second);
			}
			break;
		}
	}
	return subscriptions;
}

std::size_t CSubscriptionMgr::GetSubscriptionCount(const std::string& id) const
{
	std::shared_lock<std::shared_mutex> lock(m_mtx_info);
	auto subscriberIter = m_id_infos.find(id);
	return m_id_infos.end() == subscriberIter ? 0 : subscriberIter->second.size();
}

std::size_t CSubscriptionMgr::GetSubscriptionCount(const CQuoteInfo& info) const
{
	std::string strKey = info.String();
	if (strKey.empty())
	{
		return 0;
	}
	std::shared_lock<std::shared_mutex> lock(m_mtx_info);
	auto idsIter = m_info_ids.find(strKey);
	return m_info_ids.end() == idsIter ? 0 : idsIter->second.size();
}

bool CSubscriptionMgr::IsSubscribed(const std::string& id, const CQuoteInfo& info) const
{
	std::shared_lock<std::shared_mutex> lock(m_mtx_info);
	auto subscriberIter = m_id_infos.find(id);
	return (m_id_infos.end() != subscriberIter) && subscriberIter->second.contains(info.String());
}
