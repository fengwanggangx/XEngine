#include "TradeTypes.h"
#include <iomanip>
#include <sstream>
#include <type_traits>
namespace trade
{
	bool IsActive(Status status)
	{
		return (Status::Accepted == status) || (Status::PartiallyFilled == status);
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
		void Write(std::ostream& stream, const CAccount& value)
		{
			Write(stream, value.m_accountId);
			Write(stream, value.m_nBalance);
			Write(stream, value.m_nFrozen);
			Write(stream, value.m_bConnected);
			Write(stream, value.m_bMigrated);
		}
		void Read(std::istream& stream, CAccount& value)
		{
			Read(stream, value.m_accountId);
			Read(stream, value.m_nBalance);
			Read(stream, value.m_nFrozen);
			Read(stream, value.m_bConnected);
			Read(stream, value.m_bMigrated);
		}
		void Write(std::ostream& stream, const CPosition& value)
		{
			Write(stream, value.m_accountId);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_nTotal);
			Write(stream, value.m_nAvailable);
			Write(stream, value.m_nFrozen);
			Write(stream, value.m_nToday);
			Write(stream, value.m_nCost);
		}
		void Read(std::istream& stream, CPosition& value)
		{
			Read(stream, value.m_accountId);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_nTotal);
			Read(stream, value.m_nAvailable);
			Read(stream, value.m_nFrozen);
			Read(stream, value.m_nToday);
			Read(stream, value.m_nCost);
		}
		void Write(std::ostream& stream, const COrder& value)
		{
			Write(stream, value.m_accountId);
			Write(stream, value.m_clientId);
			Write(stream, value.m_orderId);
			Write(stream, value.m_requestId);
			Write(stream, value.m_strategyId);
			Write(stream, value.m_signalId);
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
			Write(stream, value.m_nPriceScale);
			Write(stream, value.m_nDay);
			Write(stream, value.m_strRuleVersion);
		}
		void Read(std::istream& stream, COrder& value)
		{
			Read(stream, value.m_accountId);
			Read(stream, value.m_clientId);
			Read(stream, value.m_orderId);
			Read(stream, value.m_requestId);
			Read(stream, value.m_strategyId);
			Read(stream, value.m_signalId);
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
			Read(stream, value.m_nPriceScale);
			Read(stream, value.m_nDay);
			Read(stream, value.m_strRuleVersion);
		}
		void Write(std::ostream& stream, const CTrade& value)
		{
			Write(stream, value.m_tradeId);
			Write(stream, value.m_orderId);
			Write(stream, value.m_accountId);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_side);
			Write(stream, value.m_nPrice);
			Write(stream, value.m_nQuantity);
			Write(stream, value.m_nFee);
			Write(stream, value.m_nTime);
			Write(stream, value.m_nPriceScale);
		}
		void Read(std::istream& stream, CTrade& value)
		{
			Read(stream, value.m_tradeId);
			Read(stream, value.m_orderId);
			Read(stream, value.m_accountId);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_side);
			Read(stream, value.m_nPrice);
			Read(stream, value.m_nQuantity);
			Read(stream, value.m_nFee);
			Read(stream, value.m_nTime);
			Read(stream, value.m_nPriceScale);
		}
		void Write(std::ostream& stream, const CRule& value)
		{
			Write(stream, value.m_strVersion);
			Write(stream, value.m_strSecurity);
			Write(stream, value.m_strBoard);
			Write(stream, value.m_nDay);
			Write(stream, value.m_nPriceScale);
			Write(stream, value.m_nTick);
			Write(stream, value.m_nBuyMinimum);
			Write(stream, value.m_nBuyStep);
			Write(stream, value.m_nSellMinimum);
			Write(stream, value.m_nSellStep);
			Write(stream, value.m_nLower);
			Write(stream, value.m_nUpper);
			Write(stream, value.m_nCommissionPpm);
			Write(stream, value.m_nMinimumFee);
			Write(stream, value.m_nSellTaxPpm);
			Write(stream, value.m_nTransferPpm);
			Write(stream, value.m_bCorporateAction);
		}
		void Read(std::istream& stream, CRule& value)
		{
			Read(stream, value.m_strVersion);
			Read(stream, value.m_strSecurity);
			Read(stream, value.m_strBoard);
			Read(stream, value.m_nDay);
			Read(stream, value.m_nPriceScale);
			Read(stream, value.m_nTick);
			Read(stream, value.m_nBuyMinimum);
			Read(stream, value.m_nBuyStep);
			Read(stream, value.m_nSellMinimum);
			Read(stream, value.m_nSellStep);
			Read(stream, value.m_nLower);
			Read(stream, value.m_nUpper);
			Read(stream, value.m_nCommissionPpm);
			Read(stream, value.m_nMinimumFee);
			Read(stream, value.m_nSellTaxPpm);
			Read(stream, value.m_nTransferPpm);
			Read(stream, value.m_bCorporateAction);
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
		void Write(std::ostream& stream, const CEvent& value)
		{
			Write(stream, value.m_sequence);
			Write(stream, value.m_requestId);
			Write(stream, value.m_accountId);
			Write(stream, value.m_kind);
			Write(stream, value.m_order);
			Write(stream, value.m_trade);
			Write(stream, value.m_account);
			Write(stream, value.m_position);
		}
		void Read(std::istream& stream, CEvent& value)
		{
			Read(stream, value.m_sequence);
			Read(stream, value.m_requestId);
			Read(stream, value.m_accountId);
			Read(stream, value.m_kind);
			Read(stream, value.m_order);
			Read(stream, value.m_trade);
			Read(stream, value.m_account);
			Read(stream, value.m_position);
		}
	} // namespace
	std::string EncodeState(const CState& state)
	{
		std::ostringstream stream;
		stream << "COUNTER1 ";
		Write(stream, state.m_nextOrder);
		Write(stream, state.m_nextTrade);
		Write(stream, state.m_sequence);
		Write(stream, state.m_nSettledDay);
		Write(stream, state.m_nClosedDay);
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
		Write(stream, state.m_consumedBooks.size());
		for (const auto& item : state.m_consumedBooks)
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
		if ("COUNTER1" != strMagic)
		{
			return false;
		}
		CState next;
		Read(stream, next.m_nextOrder);
		Read(stream, next.m_nextTrade);
		Read(stream, next.m_sequence);
		Read(stream, next.m_nSettledDay);
		Read(stream, next.m_nClosedDay);
		std::size_t nCount = 0;
		Read(stream, nCount);
		if (!stream || (10000000 < nCount))
		{
			return false;
		}
		for (std::size_t nIndex = 0; nIndex < nCount; ++nIndex)
		{
			CAccount value;
			Read(stream, value);
			if (!stream || !next.m_accounts.emplace(value.m_accountId, std::move(value)).second)
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
			if (!stream || !next.m_positions.emplace(std::make_pair(value.m_accountId, value.m_strSecurity), std::move(value)).second)
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
			if (!stream || !next.m_orders.emplace(value.m_orderId, std::move(value)).second)
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
			if (!stream || !next.m_trades.emplace(value.m_tradeId, std::move(value)).second)
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
			CEvent event;
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
			Id nSequence = 0;
			Read(stream, strKey);
			Read(stream, nTime);
			Read(stream, nSequence);
			if (!stream || !next.m_consumedBooks.emplace(strKey, std::make_pair(nTime, nSequence)).second)
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
	void UpdateSnapshot(CSnapshot& snapshot, const CEvent& event)
	{
		UpdateRow(snapshot.m_accounts, event.m_account, [&event](const CAccount& row)
		{
			return event.m_accountId == row.m_accountId;
		});
		switch (event.m_kind)
		{
		case EventKind::Order:
			if (0 != event.m_order.m_orderId)
			{
				UpdateRow(snapshot.m_orders, event.m_order, [&event](const COrder& row)
				{
					return event.m_order.m_orderId == row.m_orderId;
				});
			}
			break;
		case EventKind::Trade:
			UpdateRow(snapshot.m_trades, event.m_trade, [&event](const CTrade& row)
			{
				return event.m_trade.m_tradeId == row.m_tradeId;
			});
			break;
		case EventKind::Position:
			UpdateRow(snapshot.m_positions, event.m_position, [&event](const CPosition& row)
			{
				return event.m_accountId == row.m_accountId && event.m_position.m_strSecurity == row.m_strSecurity;
			});
			break;
		default:
			break;
		}
	}
	void ApplyChanges(CState& state, const CState& changes)
	{
		state.m_nextOrder = changes.m_nextOrder;
		state.m_nextTrade = changes.m_nextTrade;
		state.m_sequence = changes.m_sequence;
		state.m_nSettledDay = changes.m_nSettledDay;
		state.m_nClosedDay = changes.m_nClosedDay;
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
		for (const auto& item : changes.m_consumedBooks)
		{
			state.m_consumedBooks.insert_or_assign(item.first, item.second);
		}
		state.m_events.insert(state.m_events.end(), changes.m_events.begin(), changes.m_events.end());
	}
} // namespace trade
