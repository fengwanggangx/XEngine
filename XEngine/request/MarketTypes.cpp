#include "MarketTypes.h"

std::string GetMarketString(Exchange exchange)
{
	switch (exchange)
	{
	case Exchange::sse:
		return "SSE";
	case Exchange::szse:
		return "SZSE";
	case Exchange::bse:
		return "BSE";
	case Exchange::hkex:
		return "HKEX";
	case Exchange::cffex:
		return "CFFEX";
	case Exchange::shfe:
		return "SHFE";
	case Exchange::dce:
		return "DCE";
	case Exchange::czce:
		return "CZCE";
	case Exchange::ine:
		return "INE";
	case Exchange::gfex:
		return "GFEX";
	case Exchange::nasdaq:
		return "NASDAQ";
	case Exchange::nyse:
		return "NYSE";
	case Exchange::crypto:
		return "CRYPTO";
	default:
		return {};
	}
}

std::string GetChannelString(Channel channel)
{
	switch (channel)
	{
	case Channel::quote:
		return "quote";
	case Channel::depth:
		return "depth";
	case Channel::trade:
		return "trade";
	case Channel::bar_1m:
		return "bar_1m";
	case Channel::bar_1d:
		return "bar_1d";
	case Channel::market_status:
		return "market_status";
	default:
		return {};
	}
}

std::string GetMarketStateString(MarketState status)
{
	switch (status)
	{
	case MarketState::normal:
		return "normal";
	case MarketState::suspended:
		return "suspended";
	case MarketState::delisted:
		return "delisted";
	default:
		return "unknown";
	}
}

Exchange ParseMarket(const std::string& strExchange)
{
	if ("SSE" == strExchange)
	{
		return Exchange::sse;
	}
	if ("SZSE" == strExchange)
	{
		return Exchange::szse;
	}
	if ("BSE" == strExchange)
	{
		return Exchange::bse;
	}
	if ("HKEX" == strExchange)
	{
		return Exchange::hkex;
	}
	if ("CFFEX" == strExchange)
	{
		return Exchange::cffex;
	}
	if ("SHFE" == strExchange)
	{
		return Exchange::shfe;
	}
	if ("DCE" == strExchange)
	{
		return Exchange::dce;
	}
	if ("CZCE" == strExchange)
	{
		return Exchange::czce;
	}
	if ("INE" == strExchange)
	{
		return Exchange::ine;
	}
	if ("GFEX" == strExchange)
	{
		return Exchange::gfex;
	}
	if ("NASDAQ" == strExchange)
	{
		return Exchange::nasdaq;
	}
	if ("NYSE" == strExchange)
	{
		return Exchange::nyse;
	}
	if ("CRYPTO" == strExchange)
	{
		return Exchange::crypto;
	}
	return Exchange::unknown;
}

Channel ParseChannel(const std::string& strChannel)
{
	if ("quote" == strChannel)
	{
		return Channel::quote;
	}
	if ("depth" == strChannel)
	{
		return Channel::depth;
	}
	if ("trade" == strChannel)
	{
		return Channel::trade;
	}
	if ("bar_1m" == strChannel)
	{
		return Channel::bar_1m;
	}
	if ("bar_1d" == strChannel)
	{
		return Channel::bar_1d;
	}
	if ("market_status" == strChannel)
	{
		return Channel::market_status;
	}
	return Channel::unknown;
}

MarketState ParseMarketState(const std::string& strStatus)
{
	if ("normal" == strStatus)
	{
		return MarketState::normal;
	}
	if ("suspended" == strStatus)
	{
		return MarketState::suspended;
	}
	if ("delisted" == strStatus)
	{
		return MarketState::delisted;
	}
	return MarketState::unknown;
}

std::string FmtSecurityString(const std::string& strCode, Exchange mk)
{
	std::string strMarket = GetMarketString(mk);
	if (strCode.empty() || strMarket.empty())
	{
		return {};
	}
	return strCode + "." + strMarket;
}

CSecurity::CSecurity(const CSecurity& arg) : m_code(arg.m_code), m_name(arg.m_name), m_market(arg.m_market), m_status(arg.m_status)
{
}

CSecurity::CSecurity(const std::string& strCode, Exchange mk, MarketState status) : m_code(strCode), m_market(mk), m_status(status)
{
}

CSecurity& CSecurity::operator=(const CSecurity& arg)
{
	if (this != &arg)
	{
		m_code = arg.m_code;
		m_name = arg.m_name;
		m_market = arg.m_market;
		m_status = arg.m_status;
	}
	return *this;
}

bool CSecurity::IsValid() const
{
	return !m_code.empty() && (Exchange::unknown != m_market);
}

std::string CSecurity::String() const
{
	return FmtSecurityString(m_code, m_market);
}

bool CSecurity::operator==(const CSecurity& arg) const
{
	return (m_market == arg.m_market) && (m_code == arg.m_code);
}

bool CQuoteInfo::IsValid() const
{
	return m_security.IsValid() && (Channel::unknown != m_channel);
}

std::string CQuoteInfo::String() const
{
	return m_security.String() + ':' + GetChannelString(m_channel);
}
