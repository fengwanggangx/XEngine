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
	std::uint64_t m_connectionEpoch{ 0 };
	std::uint64_t m_fundsVersion{ 0 };
	std::uint64_t m_positionsVersion{ 0 };
	std::uint64_t m_ordersVersion{ 0 };
};

CStrategyAccountView MakeStrategyAccountView(trade::CAccountData data, const CSecurity& security);
