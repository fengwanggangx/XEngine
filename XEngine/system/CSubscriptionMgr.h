#ifndef XENGINE_SYSTEM_CSUBSCRIPTIONMGR_H
#define XENGINE_SYSTEM_CSUBSCRIPTIONMGR_H

#include "../request/MarketTypes.h"
#include <cstddef>
#include <cstdint>

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class HQOrigin
{
	Unknown,
	Client,
	Strategy
};

std::string FmtHQSubID(HQOrigin type, std::uint64_t id);
bool ParseHQSubID(const std::string& text, HQOrigin& type, std::uint64_t& id);

class CSubscriptionMgr final
{
public:
	std::vector<CQuoteInfo> Subscribe(const std::string& id, const std::vector<CQuoteInfo>& subscriptions);
	std::vector<CQuoteInfo> Unsubscribe(const std::string& id, const std::vector<CQuoteInfo>& subscriptions);
	std::vector<CQuoteInfo> RemoveSubscriber(const std::string& id);
	void RemoveSubscription(const std::string& strKey);
	std::vector<std::string> GetSubscriberIds(const std::string& strKey) const;
	std::vector<CQuoteInfo> GetSubscriptions() const;
	std::size_t GetSubscriptionCount(const std::string& id) const;
	std::size_t GetSubscriptionCount(const CQuoteInfo& info) const;
	bool IsSubscribed(const std::string& id, const CQuoteInfo& info) const;

private:
	using _TySubscriptions = std::unordered_map<std::string, CQuoteInfo>;
	using _TySubscriberIds = std::unordered_set<std::string>;

	mutable std::shared_mutex m_mtx_info;
	std::unordered_map<std::string, _TySubscriberIds> m_info_ids;
	std::unordered_map<std::string, _TySubscriptions> m_id_infos;
};

#endif
