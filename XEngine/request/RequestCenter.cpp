#include "RequestCenter.h"
#include "v1/market.pb.h"

#include <chrono>

namespace request
{
	bool IsAuthRequest(const CRequest& req)
	{
		return _TyRequest::AUTH_LOGIN == req.GetType();
	}

	bool IsRegisterAccountRequest(const CRequest& req)
	{
		return _TyRequest::AUTH_REGISTER == req.GetType();
	}

	bool IsQuerySecurityRequest(const CRequest& req)
	{
		return (req.GetType() == _TyRequest::MARKET_SECURITIES_QUERY);
	}

	bool IsSubscriptionRequest(const CRequest& req)
	{
		return (req.GetType() == _TyRequest::MARKET_SUBSCRIBE);
	}

	bool IsUnSubscriptionRequest(const CRequest& req)
	{
		return (req.GetType() == _TyRequest::MARKET_UNSUBSCRIBE);
	}

	CRequest Auth(const std::string& strUser, const std::string& strPassword)
	{
		CRequest req;
		req.SetType(_TyRequest::AUTH_LOGIN);
		req.SetExtraData("user", strUser);
		req.SetExtraData("password", strPassword);
		return req;
	}

	CRequest Auth(const std::string& strToken)
	{
		CRequest req;
		req.SetType(_TyRequest::AUTH_LOGIN);
		req.SetExtraData("token", strToken);
		return req;
	}

	CRequest Subscription(const CQuoteInfo& quote)
	{
		CRequest req;
		req.SetType(_TyRequest::MARKET_SUBSCRIBE);
		req.SetExtraData("security", quote.m_security.String());
		req.SetExtraData("channel", GetChannelString(quote.m_channel));
		return req;
	}

	CRequest UnSubscription(const CQuoteInfo& quote)
	{
		CRequest req = Subscription(quote);
		req.SetType(_TyRequest::MARKET_UNSUBSCRIBE);
		return req;
	}

	CRequest QueryQuote(const CSecurity& security)
	{
		CRequest req;
		req.SetType(_TyRequest::MARKET_QUOTE_QUERY);
		req.SetExtraData("security", security.String());
		return req;
	}

	CRequest QueryBars(const CSecurity& security, Channel channel, std::int64_t nBeginTime, std::int64_t nEndTime)
	{
		CRequest req;
		req.SetType(_TyRequest::MARKET_BARS_QUERY);
		req.SetExtraData("security", security.String());
		req.SetExtraData("channel", GetChannelString(channel));
		req.SetExtraData("begin_time_ms", std::to_string(nBeginTime));
		req.SetExtraData("end_time_ms", std::to_string(nEndTime));
		return req;
	}

	CRequest QuerySecurities()
	{
		CRequest req;
		req.SetType(_TyRequest::MARKET_SECURITIES_QUERY);
		return req;
	}

	CRequest HeartBeat()
	{
		CRequest req;
		req.SetType(_TyRequest::HEARTBEAT);
		std::int64_t nClientTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		req.SetExtraData("client_time_ms", std::to_string(nClientTime));
		return req;
	}
} // namespace request
