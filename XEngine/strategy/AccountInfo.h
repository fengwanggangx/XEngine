#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

namespace trade
{
	class ITradeCounter;
}

enum class TradeAccountType
{
	Simulated,
	Production
};

enum class TradeAccountState
{
	Disconnected,
	Connected,
	Error
};

struct CTradeAccountInfo
{
	std::uint64_t m_accountId{ 0 };
	std::string m_strName;
	TradeAccountType m_type{ TradeAccountType::Simulated };
	std::int64_t m_nInitialBalance{ 0 };
	TradeAccountState m_state{ TradeAccountState::Disconnected };
	std::string m_strNumber;
	std::uint32_t m_nStrategyCount{ 0 };
	std::string m_strLastError;
	std::string m_strCreatedAt;
	std::string m_strUpdatedAt;
	bool m_bDefault{ false };
};

struct CTradeAccount
{
	CTradeAccountInfo m_info;
	std::shared_ptr<trade::ITradeCounter> m_service;
	std::unordered_set<std::uint64_t> m_runningStrategies;
};
