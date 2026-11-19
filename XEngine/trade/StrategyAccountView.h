#pragma once

#include "TradeTypes.h"
#include "TradingTypes.h"

// All fields originate in one CTradeService::Snapshot() copy.
struct CStrategyAccountView
{
	_TyAccount m_identity;
	CAccountSnapshot m_account;
	CPositionSnapshot m_position;
	std::vector<trade::COrder> m_orders;
	std::uint64_t m_connection_epoch{ 0 };
	std::uint64_t m_funds_version{ 0 };
	std::uint64_t m_positions_version{ 0 };
	std::uint64_t m_orders_version{ 0 };
};

CStrategyAccountView MakeStrategyAccountView(trade::CAccountData data, const CSecurity& security);
