#pragma once
#include <variant>
#include "../request/AccountIdentity.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace trade
{
	enum class Error
	{
		None,
		Rejected,
		Unavailable,
		Unsupported,
		Timeout,
		UnknownResult,
		Conflict,
		NotFound
	};
	enum class Operation
	{
		Login,
		Connect,
		Disconnect,
		Submit,
		Cancel,
		Funds,
		Positions,
		Orders,
		Trades
	};
	enum class Side
	{
		Buy,
		Sell
	};
	enum class Status
	{
		Accepted,
		PartiallyFilled,
		Filled,
		Cancelled,
		Expired,
		Rejected,
		Pending,
		Unknown
	};
	enum class EventTy
	{
		Session,
		Order,
		Deal,
		Funds,
		Position
	};

	struct CResult
	{
		Error m_error{ Error::None };
		std::string m_message;

		bool Ok() const
		{
			return Error::None == m_error;
		}
	};

	struct COrder
	{
		_TyAccount m_account{};
		std::uint64_t m_client_id{ 0 }, m_order_id{ 0 }, m_request_id{ 0 }, m_id{ 0 }, m_signal_id{ 0 };
		std::string m_security;
		Side m_side{ Side::Buy };
		Status m_status{ Status::Accepted };
		std::int64_t m_price{ 0 }, m_quantity{ 0 }, m_filled{ 0 }, m_frozen{ 0 }, m_turnover{ 0 }, m_fee{ 0 }, m_time{ 0 };
		int m_price_scale{ 2 }, m_day{ 0 };
		std::string m_rule_version;
		std::string m_broker_order_id, m_raw_status, m_error_message;
		bool m_cancel_pending{ false }, m_fee_known{ true }, m_frozen_known{ true };
		std::uint32_t m_sdk_client_id{ 0 };
		std::int64_t m_broker_time{ 0 };
	};

	struct CPosition
	{
		_TyAccount m_account{};
		std::string m_security;
		std::int64_t m_total{ 0 }, m_available{ 0 }, m_frozen{ 0 }, m_today{ 0 }, m_cost{ 0 };
		bool m_frozen_known{ true }, m_cost_known{ true };
	};

	struct CFunds
	{
		_TyAccount m_account{};
		std::int64_t m_balance{ 0 }, m_frozen{ 0 };
		bool m_connected{ false }, m_migrated{ true };
		std::int64_t m_available{ 0 };
		bool m_explicit_available{ false }, m_funds_valid{ true }, m_frozen_known{ true };
		int m_state{ 0 };
	};

	struct CTrade
	{
		std::uint64_t m_trade_id{ 0 }, m_order_id{ 0 };
		_TyAccount m_account{};
		std::string m_security;
		Side m_side{ Side::Buy };
		std::int64_t m_price{ 0 }, m_quantity{ 0 }, m_fee{ 0 }, m_time{ 0 };
		int m_price_scale{ 2 };
		int m_day{ 0 };
		std::string m_broker_trade_id, m_broker_order_id;
		std::uint64_t m_report_index{ 0 };
		int m_market{ 0 };
		bool m_fee_known{ true };
	};

	struct CCounterEvent
	{
		std::uint64_t m_sequence{ 0 }, m_request_id{ 0 };
		_TyAccount m_account{};
		EventTy m_kind{ EventTy::Session };
		COrder m_order;
		CTrade m_trade;
		CFunds m_funds;
		CPosition m_position;
		bool m_account_valid{ true };
	};

	struct CLoginParam
	{
		static constexpr Operation m_operation{ Operation::Login };
		std::string m_password;
		std::string m_site;
	};

	struct CConnectParam
	{
		static constexpr Operation m_operation{ Operation::Connect };
	};

	struct CDisconnectParam
	{
		static constexpr Operation m_operation{ Operation::Disconnect };
	};

	struct CSubmitParam
	{
		static constexpr Operation m_operation{ Operation::Submit };
		COrder m_order;
	};

	struct CCancelParam
	{
		static constexpr Operation m_operation{ Operation::Cancel };
		std::uint64_t m_order_id{ 0 };
	};

	struct CFundsQueryParam
	{
		static constexpr Operation m_operation{ Operation::Funds };
	};

	struct CPositionsQueryParam
	{
		static constexpr Operation m_operation{ Operation::Positions };
	};

	struct COrdersQueryParam
	{
		static constexpr Operation m_operation{ Operation::Orders };
	};

	struct CDealsQueryParam
	{
		static constexpr Operation m_operation{ Operation::Trades };
	};

	using _TyRequestParam = std::variant<CLoginParam, CConnectParam, CDisconnectParam, CSubmitParam, CCancelParam, CFundsQueryParam, CPositionsQueryParam, COrdersQueryParam, CDealsQueryParam>;

	struct CReqParam
	{
		std::uint64_t m_request_id{ 0 };
		_TyAccount m_account{};
		_TyRequestParam m_param{ CFundsQueryParam{} };

		Operation GetOperation() const
		{
			return std::visit([](const auto& params)
			{
				return params.m_operation;
			}, m_param);
		}
	};

	struct CAccountData
	{
		bool m_ready{ false };
		std::uint64_t m_version{ 0 };
		std::int64_t m_time{ 0 };
		CFunds m_account;
		std::vector<CPosition> m_positions;
		std::vector<COrder> m_orders;
		std::vector<CTrade> m_trades;
		// Resource watermarks must not be inferred from an unrelated order event.
		std::uint64_t m_connection_epoch{ 0 };
		std::uint64_t m_funds_version{ 0 };
		std::uint64_t m_positions_version{ 0 };
		std::uint64_t m_orders_version{ 0 };
	};

	struct COperationReceipt
	{
		std::uint64_t m_order_id{ 0 };
	};

	using _TyCounterReplyData = std::variant<std::monostate, COperationReceipt, CFunds, std::vector<CPosition>, std::vector<COrder>, std::vector<CTrade>>;

	struct CCounterReply
	{
		std::uint64_t m_request_id{ 0 };
		_TyAccount m_account{};
		Operation m_operation{ Operation::Funds };
		CResult m_result;
		std::uint64_t m_version{ 0 };
		std::int64_t m_time{ 0 };
		_TyCounterReplyData m_data;
	};

	struct CRule
	{
		std::string m_version_text, m_security, m_board;
		int m_day{ 0 }, m_price_scale{ 2 };
		std::int64_t m_tick{ 1 }, m_buy_minimum{ 100 }, m_buy_step{ 100 }, m_sell_minimum{ 1 }, m_sell_step{ 1 };
		std::int64_t m_lower{ 0 }, m_upper{ 0 }, m_commission_ppm{ 0 }, m_minimum_fee{ 0 }, m_sell_tax_ppm{ 0 }, m_transfer_ppm{ 0 };
		bool m_corporate_action{ false };
	};

	struct CCalendar
	{
		int m_day{ 0 };
		std::int64_t m_open_time_ms{ 0 }, m_break_time_ms{ 0 }, m_resume_time_ms{ 0 }, m_close_time_ms{ 0 };
	};

	struct CExecutionRule
	{
		CRule m_rule;
		CCalendar m_calendar;
		bool m_reliable_query{ false };
		bool m_idempotent_client_id{ false };
	};

	struct CLevel
	{
		std::int64_t m_price{ 0 }, m_quantity{ 0 };
	};

	struct CDepth
	{
		std::string m_security;
		std::uint64_t m_sequence{ 0 };
		std::int64_t m_time{ 0 };
		int m_price_scale{ 2 };
		std::vector<CLevel> m_bids, m_asks;
	};

	struct CState
	{
		std::uint64_t m_next_order{ 1 }, m_next_trade{ 1 }, m_sequence{ 0 };
		int m_settled_day{ 0 }, m_closed_day{ 0 };
		std::map<_TyAccount, CFunds> m_accounts;
		std::map<std::pair<_TyAccount, std::string>, CPosition> m_positions;
		std::map<std::uint64_t, COrder> m_orders;
		std::map<std::uint64_t, CTrade> m_trades;
		std::vector<CCounterEvent> m_events;
		std::map<std::pair<int, std::string>, CRule> m_rules;
		std::map<int, CCalendar> m_calendar;
		std::map<std::string, std::pair<std::int64_t, std::uint64_t>> m_consumed_books;
	};

	void UpdateAccountData(CAccountData& snapshot, const CCounterEvent& event);
	void ApplyChanges(CState& state, const CState& changes);
	bool IsActive(Status status);
	std::string EncodeState(const CState& state);
	bool DecodeState(const std::string& strData, CState& state);
} // namespace trade
