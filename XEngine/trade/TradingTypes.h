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
	std::uint64_t m_client_order_id{ 0 };
	int m_n_price_scale{ 2 };
	_TyAccount m_account{};
	_TyStrategyId m_id{ 0 };
	_TySignalId m_signal_id{ 0 };
	CSecurity m_security;
	OrderSide m_side{ OrderSide::Unknown };
	OrderType m_order_type{ OrderType::Limit };
	PositionEffect m_position_effect{ PositionEffect::Unknown };
	std::int64_t m_price{ 0 };
	std::int64_t m_quantity{ 0 };
	std::string m_strReason;
	std::int64_t m_maximum_debit{ 0 };
	std::int64_t m_quote_time_ms{ 0 };
	std::string m_basis_version;

	bool IsValid() const;
};

struct COrderSubmitResult
{
	// Queue admission only; final acceptance/rejection is an order event.
	bool m_bAccepted{ false };
	bool m_bRetryable{ false };
	_TyClientOrderId m_client_order_id{ 0 };
	_TyOrderId m_order_id{ 0 };
	std::string m_strReason;
};

struct COrderEvent
{
	std::uint64_t m_sequence{ 0 };
	_TyAccount m_account{};
	_TyStrategyId m_id{ 0 };
	_TySignalId m_signal_id{ 0 };
	_TyClientOrderId m_client_order_id{ 0 };
	_TyOrderId m_order_id{ 0 };
	CSecurity m_security;
	OrderStatus m_status{ OrderStatus::Unknown };
	std::int64_t m_quantity{ 0 };
	std::int64_t m_filled_quantity{ 0 };
	std::int64_t m_event_time_ms{ 0 };
	std::string m_strReason;
};

struct CTradeEvent
{
	int m_n_price_scale{ 2 };
	std::uint64_t m_sequence{ 0 };
	_TyAccount m_account{};
	_TyStrategyId m_id{ 0 };
	_TySignalId m_signal_id{ 0 };
	_TyClientOrderId m_client_order_id{ 0 };
	_TyOrderId m_order_id{ 0 };
	std::string m_str_trade_id;
	CSecurity m_security;
	OrderSide m_side{ OrderSide::Unknown };
	std::int64_t m_price{ 0 };
	std::int64_t m_quantity{ 0 };
	std::int64_t m_trade_time_ms{ 0 };
};

struct CPositionSnapshot
{
	bool m_bReady{ false };
	std::uint64_t m_version{ 0 };
	std::int64_t m_nTime{ 0 };
	CSecurity m_security;
	std::int64_t m_total_quantity{ 0 };
	std::int64_t m_available_quantity{ 0 };
	std::int64_t m_frozen_quantity{ 0 };
	std::int64_t m_average_cost{ 0 };
	// Cost is a total amount in cents; divide only at the final comparison.
	std::int64_t m_total_cost{ 0 };
	int m_money_scale{ 2 };
	bool m_b_cost_known{ false };
	bool m_b_frozen_known{ false };
};

struct CAccountSnapshot
{
	bool m_bReady{ false };
	std::uint64_t m_version{ 0 };
	std::int64_t m_nTime{ 0 };
	std::int64_t m_balance{ 0 };
	std::int64_t m_available{ 0 };
	std::int64_t m_frozen{ 0 };
	std::int64_t m_market_value{ 0 };
	bool m_b_frozen_known{ false };
};
