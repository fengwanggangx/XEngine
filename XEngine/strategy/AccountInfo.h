#pragma once
#include "../request/AccountIdentity.h"

enum class TradeAccountState
{
	Disconnected,
	Connected,
	Error,
	Connecting,
	Synchronizing,
	Reconnecting
};

struct CAccountInfo
{
	std::uint64_t m_brokerId{ 0 };
	std::string m_strSite;
	std::string m_strAccount;
	std::string m_strBrokerName;
	std::string m_strName;
	TradeAccountState m_state{ TradeAccountState::Disconnected };
	std::string m_strLastError;
};

inline _TyAccount FmtAccount(const CAccountInfo& info)
{
	return { info.m_brokerId, info.m_strAccount };
}

inline CAccountInfo FmtAccountInfo(const _TyAccount& account)
{
	CAccountInfo info;
	info.m_brokerId = account.first;
	info.m_strAccount = account.second;
	return info;
}
