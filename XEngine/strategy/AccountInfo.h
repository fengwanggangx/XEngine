#pragma once
#include "../request/AccountIdentity.h"
#include <memory>

namespace trade
{
	class ITradeCounter;
}

enum class TradeAccountState
{
	Disconnected,
	Connected,
	Error,
	Connecting
};

struct CTradeAccountInfo
{
	std::uint64_t m_brokerId{ 0 };
	std::string m_strSite;
	std::string m_strAccount;
	std::string m_strBrokerName;
	std::string m_strName;
	TradeAccountState m_state{ TradeAccountState::Disconnected };
	std::string m_strLastError;
};

inline _TyAccountIndex AccountIndex(const CTradeAccountInfo& info)
{
	return { info.m_brokerId, info.m_strSite, info.m_strAccount };
}

inline CTradeAccountInfo AccountInfo(const _TyAccountIndex& index)
{
	CTradeAccountInfo info;
	std::tie(info.m_brokerId, info.m_strSite, info.m_strAccount) = index;
	return info;
}

struct CTradeAccount
{
	CTradeAccountInfo m_info;
	// A counter can also be retained by its adapter registry or pending operations.
	std::shared_ptr<trade::ITradeCounter> m_counter;
};
