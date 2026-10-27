#pragma once
#include "../request/MarketTypes.h"
#include <cstdint>
#include "../request/AccountIdentity.h"
#include <string>
using _TyStrategyId = std::uint64_t;
using _TySignalId = std::uint64_t;
using _TyOrderId = std::uint64_t;
using _TyClientOrderId = std::uint64_t;
enum class OrderSide
{
	Unknown,
	Buy,
	Sell
};

enum class OrderType
{
	Market,
	Limit
};

enum class PositionEffect
{
	Unknown,
	Open,
	Close,
	CloseToday
};

enum class OrderStatus
{
	Unknown,
	Pending,
	Accepted,
	Rejected,
	PartiallyFilled,
	Filled,
	Cancelling,
	Cancelled,
	Expired
};

struct COrderIntent
{
	std::uint64_t m_clientOrderId{ 0 };
	int m_nPriceScale{ 2 };
	_TyAccountIndex m_accountIndex{};
	_TyStrategyId m_strategyId{ 0 };
	_TySignalId m_signalId{ 0 };
	CSecurity m_security;
	OrderSide m_side{ OrderSide::Unknown };
	OrderType m_orderType{ OrderType::Limit };
	PositionEffect m_positionEffect{ PositionEffect::Unknown };
	std::int64_t m_price{ 0 };
	std::int64_t m_quantity{ 0 };
	std::string m_strReason;

	bool IsValid() const;
};

struct COrderSubmitResult
{
	// Queue admission only; final acceptance/rejection is an order event.
	bool m_bAccepted{ false };
	bool m_bRetryable{ false };
	_TyClientOrderId m_clientOrderId{ 0 };
	_TyOrderId m_orderId{ 0 };
	std::string m_strReason;
};

struct COrderEvent
{
	std::uint64_t m_sequence{ 0 };
	_TyAccountIndex m_accountIndex{};
	_TyStrategyId m_strategyId{ 0 };
	_TySignalId m_signalId{ 0 };
	_TyClientOrderId m_clientOrderId{ 0 };
	_TyOrderId m_orderId{ 0 };
	CSecurity m_security;
	OrderStatus m_status{ OrderStatus::Unknown };
	std::int64_t m_quantity{ 0 };
	std::int64_t m_filledQuantity{ 0 };
	std::int64_t m_eventTimeMs{ 0 };
	std::string m_strReason;
};

struct CTradeEvent
{
	int m_nPriceScale{ 2 };
	std::uint64_t m_sequence{ 0 };
	_TyAccountIndex m_accountIndex{};
	_TyStrategyId m_strategyId{ 0 };
	_TySignalId m_signalId{ 0 };
	_TyClientOrderId m_clientOrderId{ 0 };
	_TyOrderId m_orderId{ 0 };
	std::string m_strTradeId;
	CSecurity m_security;
	OrderSide m_side{ OrderSide::Unknown };
	std::int64_t m_price{ 0 };
	std::int64_t m_quantity{ 0 };
	std::int64_t m_tradeTimeMs{ 0 };
};

struct CPositionSnapshot
{
	bool m_bReady{ false };
	std::uint64_t m_version{ 0 };
	std::int64_t m_nTime{ 0 };
	CSecurity m_security;
	std::int64_t m_totalQuantity{ 0 };
	std::int64_t m_availableQuantity{ 0 };
	std::int64_t m_frozenQuantity{ 0 };
	std::int64_t m_averageCost{ 0 };
};

struct CAccountSnapshot
{
	bool m_bReady{ false };
	std::uint64_t m_version{ 0 };
	std::int64_t m_nTime{ 0 };
	std::int64_t m_balance{ 0 };
	std::int64_t m_available{ 0 };
	std::int64_t m_frozen{ 0 };
	std::int64_t m_marketValue{ 0 };
};
