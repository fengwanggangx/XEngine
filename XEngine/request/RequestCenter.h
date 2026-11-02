#ifndef XENGINE_SYSTEM_REQUESTCENTER_H
#define XENGINE_SYSTEM_REQUESTCENTER_H

#include "../request/request.h"
#include "MarketTypes.h"

#include <cstdint>
#include <string>

namespace request
{
	bool IsAuthRequest(const CRequest& req);
	bool IsRegisterAccountRequest(const CRequest& req);
	bool IsQuerySecurityRequest(const CRequest& req);
	bool IsSubscriptionRequest(const CRequest& req);
	bool IsUnSubscriptionRequest(const CRequest& req);

	CRequest Auth(const std::string& strUser, const std::string& strPassword);
	CRequest Auth(const std::string& strToken);
	CRequest Subscription(const CQuoteInfo& quote);
	CRequest UnSubscription(const CQuoteInfo& quote);
	CRequest QueryQuote(const CSecurity& security);
	CRequest QueryBars(const CSecurity& security, Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime);
	CRequest QuerySecurities();
	CRequest HeartBeat();
} // namespace request

#endif
