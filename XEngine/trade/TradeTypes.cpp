#include "TradeTypes.h"
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace trade
{
	bool IsActive(Status status)
	{
		return (Status::Accepted == status) || (Status::PartiallyFilled == status) || (Status::Pending == status) || (Status::Unknown == status);
	}

	namespace
	{
		template <class T>
		void Write(std::ostream& stream, const T& value)
		{
			if constexpr (std::is_enum_v<T>)
			{
				stream << static_cast<int>(value) << ' ';
			}
			else
			{
				stream << value << ' ';
			}
		}

		void Write(std::ostream& stream, const std::string& strValue)
		{
			stream << std::quoted(strValue) << ' ';
		}

		template <class T>
		void Read(std::istream& stream, T& value)
		{
			if constexpr (std::is_enum_v<T>)
			{
				int nValue = 0;
				stream >> nValue;
				value = static_cast<T>(nValue);
			}
			else
			{
				stream >> value;
			}
		}

		void Read(std::istream& stream, std::string& strValue)
		{
			stream >> std::quoted(strValue);
		}

		void Write(std::ostream& stream, const _TyAccount& account)
		{
			Write(stream, account.first);
			Write(stream, account.second);
		}

		void Read(std::istream& stream, _TyAccount& account)
		{
			Read(stream, account.first);
			Read(stream, account.second);
		}

		void Write(std::ostream& stream, const CFunds& value)
		{
			Write(stream, value.m_account);
			Write(stream, value.m_nBalance);
			Write(stream, value.m_nFrozen);
			Write(stream, value.m_bConnected);
			Write(stream, value.m_bMigrated);
		}

		void Read(std::istream& stream, CFunds& value)
		{
			Read(stream, value.m_account);
			Read(stream, value.m_nBalance);
			Read(stream, value.m_nFrozen);
			Read(stream, value.m_bConnected);
			Read(stream, value.m_bMigrated);
		}

		void Write(std::ostream& stream, const CPosition& value)
		{
			Write(stream, value.m_account);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_nTotal);
			Write(stream, value.m_nAvailable);
			Write(stream, value.m_nFrozen);
			Write(stream, value.m_nToday);
			Write(stream, value.m_nCost);
		}

		void Read(std::istream& stream, CPosition& value)
		{
			Read(stream, value.m_account);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_nTotal);
			Read(stream, value.m_nAvailable);
			Read(stream, value.m_nFrozen);
			Read(stream, value.m_nToday);
			Read(stream, value.m_nCost);
		}

		void Write(std::ostream& stream, const COrder& value)
		{
			Write(stream, value.m_account);
			Write(stream, value.m_client_id);
			Write(stream, value.m_order_id);
			Write(stream, value.m_request_id);
			Write(stream, value.m_id);
			Write(stream, value.m_signal_id);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_side);
			Write(stream, value.m_status);
			Write(stream, value.m_nPrice);
			Write(stream, value.m_nQuantity);
			Write(stream, value.m_nFilled);
			Write(stream, value.m_nFrozen);
			Write(stream, value.m_nTurnover);
			Write(stream, value.m_nFee);
			Write(stream, value.m_nTime);
			Write(stream, value.m_n_price_scale);
			Write(stream, value.m_nDay);
			Write(stream, value.m_str_rule_version);
		}

		void Read(std::istream& stream, COrder& value)
		{
			Read(stream, value.m_account);
			Read(stream, value.m_client_id);
			Read(stream, value.m_order_id);
			Read(stream, value.m_request_id);
			Read(stream, value.m_id);
			Read(stream, value.m_signal_id);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_side);
			Read(stream, value.m_status);
			Read(stream, value.m_nPrice);
			Read(stream, value.m_nQuantity);
			Read(stream, value.m_nFilled);
			Read(stream, value.m_nFrozen);
			Read(stream, value.m_nTurnover);
			Read(stream, value.m_nFee);
			Read(stream, value.m_nTime);
			Read(stream, value.m_n_price_scale);
			Read(stream, value.m_nDay);
			Read(stream, value.m_str_rule_version);
		}

		void Write(std::ostream& stream, const CTrade& value)
		{
			Write(stream, value.m_trade_id);
			Write(stream, value.m_order_id);
			Write(stream, value.m_account);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_side);
			Write(stream, value.m_nPrice);
			Write(stream, value.m_nQuantity);
			Write(stream, value.m_nFee);
			Write(stream, value.m_nTime);
			Write(stream, value.m_n_price_scale);
		}

		void Read(std::istream& stream, CTrade& value)
		{
			Read(stream, value.m_trade_id);
			Read(stream, value.m_order_id);
			Read(stream, value.m_account);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_side);
			Read(stream, value.m_nPrice);
			Read(stream, value.m_nQuantity);
			Read(stream, value.m_nFee);
			Read(stream, value.m_nTime);
			Read(stream, value.m_n_price_scale);
		}

		void Write(std::ostream& stream, const CRule& value)
		{
			Write(stream, value.m_strVersion);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_strBoard);
			Write(stream, value.m_nDay);
			Write(stream, value.m_n_price_scale);
			Write(stream, value.m_nTick);
			Write(stream, value.m_n_buy_minimum);
			Write(stream, value.m_n_buy_step);
			Write(stream, value.m_n_sell_minimum);
			Write(stream, value.m_n_sell_step);
			Write(stream, value.m_nLower);
			Write(stream, value.m_nUpper);
			Write(stream, value.m_n_commission_ppm);
			Write(stream, value.m_n_minimum_fee);
			Write(stream, value.m_n_sell_tax_ppm);
			Write(stream, value.m_n_transfer_ppm);
			Write(stream, value.m_b_corporate_action);
		}

		void Read(std::istream& stream, CRule& value)
		{
			Read(stream, value.m_strVersion);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_strBoard);
			Read(stream, value.m_nDay);
			Read(stream, value.m_n_price_scale);
			Read(stream, value.m_nTick);
			Read(stream, value.m_n_buy_minimum);
			Read(stream, value.m_n_buy_step);
			Read(stream, value.m_n_sell_minimum);
			Read(stream, value.m_n_sell_step);
			Read(stream, value.m_nLower);
			Read(stream, value.m_nUpper);
			Read(stream, value.m_n_commission_ppm);
			Read(stream, value.m_n_minimum_fee);
			Read(stream, value.m_n_sell_tax_ppm);
			Read(stream, value.m_n_transfer_ppm);
			Read(stream, value.m_b_corporate_action);
		}

		void Write(std::ostream& stream, const CCalendar& value)
		{
			Write(stream, value.m_nDay);
			Write(stream, value.m_nOpen);
			Write(stream, value.m_nBreak);
			Write(stream, value.m_nResume);
			Write(stream, value.m_nClose);
		}

		void Read(std::istream& stream, CCalendar& value)
		{
			Read(stream, value.m_nDay);
			Read(stream, value.m_nOpen);
			Read(stream, value.m_nBreak);
			Read(stream, value.m_nResume);
			Read(stream, value.m_nClose);
		}

		void Write(std::ostream& stream, const CCounterEvent& value)
		{
			Write(stream, value.m_sequence);
			Write(stream, value.m_request_id);
			Write(stream, value.m_account);
			Write(stream, value.m_kind);
			Write(stream, value.m_order);
			Write(stream, value.m_trade);
			Write(stream, value.m_funds);
			Write(stream, value.m_position);
		}

		void Read(std::istream& stream, CCounterEvent& value)
		{
			Read(stream, value.m_sequence);
			Read(stream, value.m_request_id);
			Read(stream, value.m_account);
			Read(stream, value.m_kind);
			Read(stream, value.m_order);
			Read(stream, value.m_trade);
			Read(stream, value.m_funds);
			Read(stream, value.m_position);
		}
	} // namespace

	std::string EncodeState(const CState& state)
	{
		std::ostringstream stream;
		stream << "COUNTER3 ";
		Write(stream, state.m_next_order);
		Write(stream, state.m_next_trade);
		Write(stream, state.m_sequence);
		Write(stream, state.m_n_settled_day);
		Write(stream, state.m_n_closed_day);
		Write(stream, state.m_accounts.size());
		for (const auto& item : state.m_accounts)
		{
			Write(stream, item.second);
		}
		Write(stream, state.m_positions.size());
		for (const auto& item : state.m_positions)
		{
			Write(stream, item.second);
		}
		Write(stream, state.m_orders.size());
		for (const auto& item : state.m_orders)
		{
			Write(stream, item.second);
		}
		Write(stream, state.m_trades.size());
		for (const auto& item : state.m_trades)
		{
			Write(stream, item.second);
		}
		Write(stream, state.m_rules.size());
		for (const auto& item : state.m_rules)
		{
			Write(stream, item.second);
		}
		Write(stream, state.m_calendar.size());
		for (const auto& item : state.m_calendar)
		{
			Write(stream, item.second);
		}
		Write(stream, state.m_events.size());
		for (const auto& event : state.m_events)
		{
			Write(stream, event);
		}
		Write(stream, state.m_consumed_books.size());
		for (const auto& item : state.m_consumed_books)
		{
			Write(stream, item.first);
			Write(stream, item.second.first);
			Write(stream, item.second.second);
		}
		return stream.str();
	}

	bool DecodeState(const std::string& strData, CState& state)
	{
		std::istringstream stream(strData);
		std::string strMagic;
		stream >> strMagic;
		if ("COUNTER3" != strMagic)
		{
			return false;
		}
		CState next;
		Read(stream, next.m_next_order);
		Read(stream, next.m_next_trade);
		Read(stream, next.m_sequence);
		Read(stream, next.m_n_settled_day);
		Read(stream, next.m_n_closed_day);
		std::size_t nCount = 0;
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CFunds value;
			Read(stream, value);
			if (!stream || !next.m_accounts.emplace(value.m_account, std::move(value)).second)
			{
				return false;
			}
		}
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CPosition value;
			Read(stream, value);
			if (!stream || !next.m_positions.emplace(std::make_pair(value.m_account, value.m_strSecurity), std::move(value)).second)
			{
				return false;
			}
		}
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			COrder value;
			Read(stream, value);
			if (!stream || !next.m_orders.emplace(value.m_order_id, std::move(value)).second)
			{
				return false;
			}
		}
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CTrade value;
			Read(stream, value);
			if (!stream || !next.m_trades.emplace(value.m_trade_id, std::move(value)).second)
			{
				return false;
			}
		}
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CRule value;
			Read(stream, value);
			if (!stream || !next.m_rules.emplace(std::make_pair(value.m_nDay, value.m_strSecurity), std::move(value)).second)
			{
				return false;
			}
		}
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CCalendar value;
			Read(stream, value);
			if (!stream || !next.m_calendar.emplace(value.m_nDay, std::move(value)).second)
			{
				return false;
			}
		}
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		next.m_events.reserve(nCount);
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CCounterEvent event;
			Read(stream, event);
			if (!stream)
			{
				return false;
			}
			next.m_events.emplace_back(std::move(event));
		}
		Read(stream, nCount);
		if (!stream || 10000000 < nCount)
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			std::string strKey;
			std::int64_t nTime = 0;
			std::uint64_t nSequence = 0;
			Read(stream, strKey);
			Read(stream, nTime);
			Read(stream, nSequence);
			if (!stream || !next.m_consumed_books.emplace(strKey, std::make_pair(nTime, nSequence)).second)
			{
				return false;
			}
		}
		stream >> std::ws;
		if (!stream.eof())
		{
			return false;
		}
		state = std::move(next);
		return true;
	}

	namespace
	{
		template <class Row, class Match>
		void UpdateRow(std::vector<Row>& rows, const Row& row, Match match)
		{
			for (auto& existing : rows)
			{
				if (match(existing))
				{
					existing = row;
					return;
				}
			}
			rows.push_back(row);
		}
	} // namespace

	void UpdateAccountData(CAccountData& snapshot, const CCounterEvent& event)
	{
		if (!IsAccountValid(snapshot.m_account.m_account) || snapshot.m_account.m_account != event.m_account)
		{
			return;
		}
		if (event.m_b_account_valid)
		{
			snapshot.m_account = event.m_funds;
		}
		snapshot.m_version = event.m_sequence;
		switch (event.m_kind)
		{
		case EventTy::Order:
			if (0 != event.m_order.m_order_id)
			{
				UpdateRow(snapshot.m_orders, event.m_order, [&event](const COrder& row)
				{
					return event.m_order.m_order_id == row.m_order_id;
				});
			}
			break;
		case EventTy::Deal:
			UpdateRow(snapshot.m_trades, event.m_trade, [&event](const CTrade& row)
			{
				return event.m_trade.m_trade_id == row.m_trade_id;
			});
			break;
		case EventTy::Position:
			UpdateRow(snapshot.m_positions, event.m_position, [&event](const CPosition& row)
			{
				return event.m_account == row.m_account && event.m_position.m_strSecurity == row.m_strSecurity;
			});
			break;
		default:
			break;
		}
	}

	void ApplyChanges(CState& state, const CState& changes)
	{
		state.m_next_order = changes.m_next_order;
		state.m_next_trade = changes.m_next_trade;
		state.m_sequence = changes.m_sequence;
		state.m_n_settled_day = changes.m_n_settled_day;
		state.m_n_closed_day = changes.m_n_closed_day;
		for (const auto& item : changes.m_accounts)
		{
			state.m_accounts.insert_or_assign(item.first, item.second);
		}
		for (const auto& item : changes.m_positions)
		{
			state.m_positions.insert_or_assign(item.first, item.second);
		}
		for (const auto& item : changes.m_orders)
		{
			state.m_orders.insert_or_assign(item.first, item.second);
		}
		for (const auto& item : changes.m_trades)
		{
			state.m_trades.insert_or_assign(item.first, item.second);
		}
		for (const auto& item : changes.m_rules)
		{
			state.m_rules.insert_or_assign(item.first, item.second);
		}
		for (const auto& item : changes.m_calendar)
		{
			state.m_calendar.insert_or_assign(item.first, item.second);
		}
		for (const auto& item : changes.m_consumed_books)
		{
			state.m_consumed_books.insert_or_assign(item.first, item.second);
		}
		state.m_events.insert(state.m_events.end(), changes.m_events.begin(), changes.m_events.end());
	}
} // namespace trade
