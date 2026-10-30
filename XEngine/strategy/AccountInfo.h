#pragma once
#include "../request/AccountIdentity.h"
#include <memory>

namespace trade
{
	class ITradeSession;
}

enum class TradeAccountState
{
	Disconnected,
	Connected,
	Error,
	Connecting,
	Synchronizing,
	Reconnecting
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

inline _TyAccount AccountIndex(const CTradeAccountInfo& info)
{
	return { info.m_brokerId, info.m_strAccount };
}

inline CTradeAccountInfo AccountInfo(const _TyAccount& index)
{
	CTradeAccountInfo info;
	info.m_brokerId = index.first;
	info.m_strAccount = index.second;
	return info;
}

struct CTradeAccount
{
	CTradeAccountInfo m_info;
	// Pending operations may retain the account session until their response completes.
	std::shared_ptr<trade::ITradeSession> m_session;
};
