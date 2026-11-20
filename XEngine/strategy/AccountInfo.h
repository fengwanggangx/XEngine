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

// 保存交易账户的身份、展示信息、连接状态及最近错误。
struct CAccountInfo
{
	std::uint64_t m_broker_id{ 0 };
	std::string m_site;
	std::string m_account;
	std::string m_broker_name;
	std::string m_name;
	TradeAccountState m_state{ TradeAccountState::Disconnected };
	std::string m_last_error;
};

inline _TyAccount FmtAccount(const CAccountInfo& info)
{
	return { info.m_broker_id, info.m_account };
}

inline CAccountInfo FmtAccountInfo(const _TyAccount& account)
{
	CAccountInfo info;
	info.m_broker_id = account.first;
	info.m_account = account.second;
	return info;
}
