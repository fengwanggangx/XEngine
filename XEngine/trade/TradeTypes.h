#pragma once
#include <variant>
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
		Trades,
		Synchronize
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
		Rejected
	};
	enum class EventKind
	{
		Session,
		Order,
		Trade,
		Funds,
		Position
	};
	struct CResult
	{
		Error m_error{ Error::None };
		std::string m_strMessage;
		bool Ok() const
		{
			return Error::None == m_error;
		}
	};
	struct COrder
	{
		std::uint64_t m_accountId{ 0 }, m_clientId{ 0 }, m_orderId{ 0 }, m_requestId{ 0 }, m_strategyId{ 0 }, m_signalId{ 0 };
		std::string m_strSecurity;
		Side m_side{ Side::Buy };
		Status m_status{ Status::Accepted };
		std::int64_t m_nPrice{ 0 }, m_nQuantity{ 0 }, m_nFilled{ 0 }, m_nFrozen{ 0 }, m_nTurnover{ 0 }, m_nFee{ 0 }, m_nTime{ 0 };
		int m_nPriceScale{ 2 }, m_nDay{ 0 };
		std::string m_strRuleVersion;
	};
	struct CPosition
	{
		std::uint64_t m_accountId{ 0 };
		std::string m_strSecurity;
		std::int64_t m_nTotal{ 0 }, m_nAvailable{ 0 }, m_nFrozen{ 0 }, m_nToday{ 0 }, m_nCost{ 0 };
	};
	struct CAccount
	{
		std::uint64_t m_accountId{ 0 };
		std::int64_t m_nBalance{ 0 }, m_nFrozen{ 0 };
		bool m_bConnected{ false }, m_bMigrated{ true };
	};
	struct CTrade
	{
		std::uint64_t m_tradeId{ 0 }, m_orderId{ 0 }, m_accountId{ 0 };
		std::string m_strSecurity;
		Side m_side{ Side::Buy };
		std::int64_t m_nPrice{ 0 }, m_nQuantity{ 0 }, m_nFee{ 0 }, m_nTime{ 0 };
		int m_nPriceScale{ 2 };
	};
	struct CEvent
	{
		std::uint64_t m_sequence{ 0 }, m_requestId{ 0 }, m_accountId{ 0 };
		EventKind m_kind{ EventKind::Session };
		COrder m_order;
		CTrade m_trade;
		CAccount m_account;
		CPosition m_position;
	};
	struct CLoginParams
	{
		static constexpr Operation m_operation{ Operation::Login };
		std::string m_strBroker, m_strSite, m_strUser, m_strPassword;
	};

	struct CConnect
	{
		static constexpr Operation m_operation{ Operation::Connect };
	};

	struct CDisconnect
	{
		static constexpr Operation m_operation{ Operation::Disconnect };
	};

	struct CSubmitParams
	{
		static constexpr Operation m_operation{ Operation::Submit };
		COrder m_order;
	};

	struct CCancelParams
	{
		static constexpr Operation m_operation{ Operation::Cancel };
		std::uint64_t m_orderId{ 0 };
	};

	struct CFundsQuery
	{
		static constexpr Operation m_operation{ Operation::Funds };
	};

	struct CPositionsQuery
	{
		static constexpr Operation m_operation{ Operation::Positions };
	};

	struct COrdersQuery
	{
		static constexpr Operation m_operation{ Operation::Orders };
	};

	struct CTradesQuery
	{
		static constexpr Operation m_operation{ Operation::Trades };
	};

	struct CSynchronize
	{
		static constexpr Operation m_operation{ Operation::Synchronize };
	};

	using _TyRequestParams = std::variant<CLoginParams, CConnect, CDisconnect, CSubmitParams, CCancelParams, CFundsQuery, CPositionsQuery, COrdersQuery, CTradesQuery, CSynchronize>;

	struct CRequest
	{
		std::uint64_t m_requestId{ 0 }, m_accountId{ 0 };
		_TyRequestParams m_params{ CSynchronize{} };

		Operation GetOperation() const
		{
			return std::visit([](const auto& params)
			{
				return params.m_operation;
			}, m_params);
		}
	};
	struct CAccountData
	{
		bool m_bReady{ false };
		std::uint64_t m_version{ 0 };
		std::int64_t m_nTime{ 0 };
		CAccount m_account;
		std::vector<CPosition> m_positions;
		std::vector<COrder> m_orders;
		std::vector<CTrade> m_trades;
	};
	struct COperationReceipt
	{
		std::uint64_t m_orderId{ 0 };
	};
	using CounterReplyData = std::variant<std::monostate, COperationReceipt, CAccount, std::vector<CPosition>, std::vector<COrder>, std::vector<CTrade>, CAccountData>;
	struct CCounterReply
	{
		std::uint64_t m_requestId{ 0 }, m_accountId{ 0 };
		Operation m_operation{ Operation::Synchronize };
		CResult m_result;
		std::uint64_t m_version{ 0 };
		std::int64_t m_nTime{ 0 };
		CounterReplyData m_data;
	};
	struct CRule
	{
		std::string m_strVersion, m_strSecurity, m_strBoard;
		int m_nDay{ 0 }, m_nPriceScale{ 2 };
		std::int64_t m_nTick{ 1 }, m_nBuyMinimum{ 100 }, m_nBuyStep{ 100 }, m_nSellMinimum{ 1 }, m_nSellStep{ 1 };
		std::int64_t m_nLower{ 0 }, m_nUpper{ 0 }, m_nCommissionPpm{ 0 }, m_nMinimumFee{ 0 }, m_nSellTaxPpm{ 0 }, m_nTransferPpm{ 0 };
		bool m_bCorporateAction{ false };
	};
	struct CCalendar
	{
		int m_nDay{ 0 };
		std::int64_t m_nOpen{ 0 }, m_nBreak{ 0 }, m_nResume{ 0 }, m_nClose{ 0 };
	};
	struct CLevel
	{
		std::int64_t m_nPrice{ 0 }, m_nQuantity{ 0 };
	};
	struct CDepth
	{
		std::string m_strSecurity;
		std::uint64_t m_sequence{ 0 };
		std::int64_t m_nTime{ 0 };
		int m_nPriceScale{ 2 };
		std::vector<CLevel> m_bids, m_asks;
	};
	struct CState
	{
		std::uint64_t m_nextOrder{ 1 }, m_nextTrade{ 1 }, m_sequence{ 0 };
		int m_nSettledDay{ 0 }, m_nClosedDay{ 0 };
		std::map<std::uint64_t, CAccount> m_accounts;
		std::map<std::pair<std::uint64_t, std::string>, CPosition> m_positions;
		std::map<std::uint64_t, COrder> m_orders;
		std::map<std::uint64_t, CTrade> m_trades;
		std::vector<CEvent> m_events;
		std::map<std::pair<int, std::string>, CRule> m_rules;
		std::map<int, CCalendar> m_calendar;
		std::map<std::string, std::pair<std::int64_t, std::uint64_t>> m_consumedBooks;
	};
	void UpdateAccountData(CAccountData& snapshot, const CEvent& event);
	void ApplyChanges(CState& state, const CState& changes);
	bool IsActive(Status status);
	std::string EncodeState(const CState& state);
	bool DecodeState(const std::string& strData, CState& state);
} // namespace trade
