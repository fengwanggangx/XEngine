#include "TradeProtocol.h"
#include "../common/utility.h"
namespace trade::protocol
{
	void WriteOrder(const trade::COrder& row, request::TradeOrderRecord& wire)
	{
		wire.set_account_id(row.m_accountId);
		wire.set_client_order_id(row.m_clientId);
		wire.set_order_id(row.m_orderId);
		wire.set_request_id(row.m_requestId);
		wire.set_strategy_id(row.m_strategyId);
		wire.set_signal_id(row.m_signalId);
		wire.set_security(row.m_strSecurity);
		wire.set_side(static_cast<int>(row.m_side));
		wire.set_state(static_cast<int>(row.m_status));
		wire.set_price(row.m_nPrice);
		wire.set_price_scale(row.m_nPriceScale);
		wire.set_quantity(row.m_nQuantity);
		wire.set_filled_quantity(row.m_nFilled);
		wire.set_frozen(row.m_nFrozen);
		wire.set_turnover(row.m_nTurnover);
		wire.set_fee(row.m_nFee);
		wire.set_time_ms(row.m_nTime);
		wire.set_rule_version(row.m_strRuleVersion);
	}
	void WriteTrade(const trade::CTrade& row, request::TradeRecord& wire)
	{
		wire.set_account_id(row.m_accountId);
		wire.set_trade_id(row.m_tradeId);
		wire.set_order_id(row.m_orderId);
		wire.set_security(row.m_strSecurity);
		wire.set_side(static_cast<int>(row.m_side));
		wire.set_price(row.m_nPrice);
		wire.set_price_scale(row.m_nPriceScale);
		wire.set_quantity(row.m_nQuantity);
		wire.set_fee(row.m_nFee);
		wire.set_time_ms(row.m_nTime);
	}
	void WritePosition(const trade::CPosition& row, request::TradePositionRecord& wire)
	{
		wire.set_account_id(row.m_accountId);
		wire.set_security(row.m_strSecurity);
		wire.set_total_quantity(row.m_nTotal);
		wire.set_available_quantity(row.m_nAvailable);
		wire.set_frozen_quantity(row.m_nFrozen);
		wire.set_today_quantity(row.m_nToday);
		wire.set_book_cost_cents(row.m_nCost);
	}
	CSecurity Security(const std::string& strValue)
	{
		std::size_t nDot = strValue.rfind('.');
		return std::string::npos == nDot ? CSecurity{} : CSecurity(strValue.substr(0, nDot), ParseMarket(strValue.substr(nDot + 1)));
	}
	void WriteFunds(const CAccount& row, bool bReady, request::TradeAccountInfo& wire)
	{
		wire.set_account_id(row.m_accountId);
		wire.set_balance(row.m_nBalance);
		wire.set_frozen(row.m_nFrozen);
		wire.set_available(row.m_nBalance - row.m_nFrozen);
		wire.set_available_snapshot(bReady && row.m_bConnected);
	}
	void WriteEvent(const CEvent& event, request::TradeEventRecord& wire)
	{
		wire.set_sequence(event.m_sequence);
		wire.set_request_id(event.m_requestId);
		wire.set_account_id(event.m_accountId);
		wire.set_kind(static_cast<int>(event.m_kind));
		wire.set_connected(event.m_account.m_bConnected);
		switch (event.m_kind)
		{
		case EventKind::Order:
			WriteOrder(event.m_order, *wire.mutable_order());
			break;
		case EventKind::Trade:
			WriteTrade(event.m_trade, *wire.mutable_trade());
			break;
		case EventKind::Position:
			WritePosition(event.m_position, *wire.mutable_position());
			break;
		case EventKind::Funds:
			WriteFunds(event.m_account, true, *wire.mutable_funds());
			break;
		default:
			break;
		}
	}
	COrderEvent OrderEvent(const CEvent& event)
	{
		const trade::COrder& row = event.m_order;
		COrderEvent order;
		order.m_sequence = event.m_sequence;
		order.m_accountId = event.m_accountId;
		order.m_strategyId = row.m_strategyId;
		order.m_signalId = row.m_signalId;
		order.m_clientOrderId = row.m_clientId;
		order.m_orderId = row.m_orderId;
		order.m_security = Security(row.m_strSecurity);
		order.m_quantity = row.m_nQuantity;
		order.m_filledQuantity = row.m_nFilled;
		order.m_eventTimeMs = row.m_nTime;
		switch (row.m_status)
		{
		case trade::Status::Accepted:
			order.m_status = OrderStatus::Accepted;
			break;
		case trade::Status::PartiallyFilled:
			order.m_status = OrderStatus::PartiallyFilled;
			break;
		case trade::Status::Filled:
			order.m_status = OrderStatus::Filled;
			break;
		case trade::Status::Cancelled:
			order.m_status = OrderStatus::Cancelled;
			break;
		case trade::Status::Expired:
			order.m_status = OrderStatus::Expired;
			break;
		default:
			order.m_status = OrderStatus::Rejected;
		}
		return order;
	}
	CTradeEvent TradeEvent(const CEvent& event)
	{
		CTradeEvent trade;
		trade.m_sequence = event.m_sequence;
		trade.m_accountId = event.m_accountId;
		trade.m_strategyId = event.m_order.m_strategyId;
		trade.m_signalId = event.m_order.m_signalId;
		trade.m_clientOrderId = event.m_order.m_clientId;
		trade.m_orderId = event.m_order.m_orderId;
		trade.m_strTradeId = std::to_string(event.m_trade.m_tradeId);
		trade.m_security = Security(event.m_trade.m_strSecurity);
		trade.m_side = trade::Side::Buy == event.m_trade.m_side ? OrderSide::Buy : OrderSide::Sell;
		trade.m_price = event.m_trade.m_nPrice;
		trade.m_nPriceScale = event.m_trade.m_nPriceScale;
		trade.m_quantity = event.m_trade.m_nQuantity;
		trade.m_tradeTimeMs = event.m_trade.m_nTime;
		return trade;
	}
	request::TradeAccountRecord AccountPayload(const CTradeAccountInfo& info)
	{
		request::TradeAccountRecord payload;
		payload.set_account_id(info.m_accountId);
		payload.set_name(info.m_strName);
		payload.set_account_type(TradeAccountType::Simulated == info.m_type ? request::ACCOUNT_SIMULATED : request::ACCOUNT_PRODUCTION);
		payload.set_initial_balance(info.m_nInitialBalance);
		switch (info.m_state)
		{
		case TradeAccountState::Disconnected:
			payload.set_state(request::ACCOUNT_DISCONNECTED);
			break;
		case TradeAccountState::Connected:
			payload.set_state(request::ACCOUNT_CONNECTED);
			break;
		case TradeAccountState::Error:
			payload.set_state(request::ACCOUNT_ERROR);
			break;
		}
		payload.set_number(info.m_strNumber);
		payload.set_strategy_count(info.m_nStrategyCount);
		payload.set_last_error(info.m_strLastError);
		payload.set_created_at(info.m_strCreatedAt);
		payload.set_updated_at(info.m_strUpdatedAt);
		payload.set_is_default(info.m_bDefault);
		return payload;
	}
	::CRequest Push(const CEvent& event, const CTradeAccountInfo& account)
	{
		::CRequest push;
		push.SetId(0);
		push.SetExtraData("sequence", std::to_string(event.m_sequence));
		push.SetExtraData("request_id", std::to_string(event.m_requestId));
		push.SetExtraData("account_id", std::to_string(event.m_accountId));
		switch (event.m_kind)
		{
		case trade::EventKind::Session:
			push.SetType(_TyRequest::TRADE_ACCOUNT_STATE_PUSH);
			push.SetData(AccountPayload(account));
			break;
		case trade::EventKind::Order:
		{
			push.SetType(_TyRequest::TRADE_ORDER_PUSH);
			request::TradeOrderList rows;
			WriteOrder(event.m_order, *rows.add_orders());
			push.SetData(rows);
		}
		break;
		case trade::EventKind::Trade:
		{
			push.SetType(_TyRequest::TRADE_TRADE_PUSH);
			request::TradeList rows;
			WriteTrade(event.m_trade, *rows.add_trades());
			push.SetData(rows);
		}
		break;
		case trade::EventKind::Funds:
		{
			push.SetType(_TyRequest::TRADE_ACCOUNT_INFO_PUSH);
			request::TradeAccountInfoList rows;
			WriteFunds(event.m_account, true, *rows.add_accounts());
			push.SetData(rows);
		}
		break;
		case trade::EventKind::Position:
		{
			push.SetType(_TyRequest::TRADE_POSITION_PUSH);
			request::TradePositionList rows;
			WritePosition(event.m_position, *rows.add_positions());
			push.SetData(rows);
		}
		break;
		}
		return push;
	}
} // namespace trade::protocol

namespace trade::protocol
{
	namespace
	{
		COrder ReadOrder(const request::TradeOrderRecord& wire)
		{
			COrder row;
			row.m_accountId = wire.account_id();
			row.m_clientId = wire.client_order_id();
			row.m_orderId = wire.order_id();
			row.m_requestId = wire.request_id();
			row.m_strategyId = wire.strategy_id();
			row.m_signalId = wire.signal_id();
			row.m_strSecurity = wire.security();
			row.m_nPrice = wire.price();
			row.m_nPriceScale = wire.price_scale();
			row.m_nQuantity = wire.quantity();
			row.m_nFilled = wire.filled_quantity();
			row.m_nFrozen = wire.frozen();
			row.m_nTurnover = wire.turnover();
			row.m_nFee = wire.fee();
			row.m_nTime = wire.time_ms();
			row.m_strRuleVersion = wire.rule_version();
			row.m_side = static_cast<Side>(wire.side());
			row.m_status = static_cast<Status>(wire.state());
			return row;
		}
		CTrade ReadTrade(const request::TradeRecord& wire)
		{
			CTrade row;
			row.m_accountId = wire.account_id();
			row.m_orderId = wire.order_id();
			row.m_tradeId = wire.trade_id();
			row.m_strSecurity = wire.security();
			row.m_nPrice = wire.price();
			row.m_nPriceScale = wire.price_scale();
			row.m_nQuantity = wire.quantity();
			row.m_nFee = wire.fee();
			row.m_nTime = wire.time_ms();
			row.m_side = static_cast<Side>(wire.side());
			return row;
		}
		CPosition ReadPosition(const request::TradePositionRecord& wire)
		{
			return { wire.account_id(), wire.security(), wire.total_quantity(), wire.available_quantity(), wire.frozen_quantity(), wire.today_quantity(), wire.book_cost_cents() };
		}
		CAccount ReadFunds(const request::TradeAccountInfo& wire)
		{
			return { wire.account_id(), wire.balance(), wire.frozen(), wire.available_snapshot(), true };
		}
		bool ReadEvent(const request::TradeEventRecord& wire, CEvent& event)
		{
			if (0 > wire.kind() || static_cast<int>(EventKind::Position) < wire.kind())
			{
				return false;
			}
			event.m_sequence = wire.sequence();
			event.m_accountId = wire.account_id();
			event.m_requestId = wire.request_id();
			event.m_kind = static_cast<EventKind>(wire.kind());
			event.m_account.m_accountId = event.m_accountId;
			event.m_account.m_bConnected = wire.connected();
			switch (event.m_kind)
			{
			case EventKind::Order:
				if (!wire.has_order())
				{
					return false;
				}
				event.m_order = ReadOrder(wire.order());
				break;
			case EventKind::Trade:
				if (!wire.has_trade())
				{
					return false;
				}
				event.m_trade = ReadTrade(wire.trade());
				break;
			case EventKind::Funds:
				if (!wire.has_funds())
				{
					return false;
				}
				event.m_account = ReadFunds(wire.funds());
				break;
			case EventKind::Position:
				if (!wire.has_position())
				{
					return false;
				}
				event.m_position = ReadPosition(wire.position());
				break;
			case EventKind::Session:
				break;
			}
			return true;
		}
	} // namespace
	::CRequest WriteRequest(const CRequest& request)
	{
		::CRequest wire;
		wire.SetId(request.m_requestId);
		wire.SetExtraData("account_id", std::to_string(request.m_accountId));
		switch (request.m_operation)
		{
		case Operation::Login:
			wire.SetType(_TyRequest::TRADE_ACCOUNT_LOGIN);
			wire.SetExtraData("broker", request.m_strBroker);
			wire.SetExtraData("site", request.m_strSite);
			wire.SetExtraData("account", request.m_strUser);
			wire.SetExtraData("password", request.m_strPassword);
			break;
		case Operation::Connect:
			wire.SetType(_TyRequest::TRADE_ACCOUNT_CONNECT);
			break;
		case Operation::Disconnect:
			wire.SetType(_TyRequest::TRADE_ACCOUNT_DISCONNECT);
			break;
		case Operation::Submit:
			wire.SetType(_TyRequest::TRADE_ORDER_SUBMIT);
			wire.SetExtraData("client_order_id", std::to_string(request.m_order.m_clientId));
			wire.SetExtraData("security", request.m_order.m_strSecurity);
			wire.SetExtraData("side", Side::Buy == request.m_order.m_side ? "buy" : "sell");
			wire.SetExtraData("order_type", "limit");
			wire.SetExtraData("price", std::to_string(request.m_order.m_nPrice));
			wire.SetExtraData("price_scale", std::to_string(request.m_order.m_nPriceScale));
			wire.SetExtraData("quantity", std::to_string(request.m_order.m_nQuantity));
			wire.SetExtraData("strategy_id", std::to_string(request.m_order.m_strategyId));
			wire.SetExtraData("signal_id", std::to_string(request.m_order.m_signalId));
			break;
		case Operation::Cancel:
			wire.SetType(_TyRequest::TRADE_ORDER_CANCEL);
			wire.SetExtraData("order_id", std::to_string(request.m_orderId));
			break;
		case Operation::Funds:
			wire.SetType(_TyRequest::TRADE_ACCOUNT_INFO_QUERY);
			if (0 != request.m_accountId)
			{
				wire.SetExtraData("account_ids", std::vector<std::uint64_t>{ request.m_accountId });
			}
			break;
		case Operation::Positions:
			wire.SetType(_TyRequest::TRADE_POSITION_QUERY);
			wire.SetExtraData("security", request.m_strSecurity);
			break;
		case Operation::Orders:
			wire.SetType(_TyRequest::TRADE_ORDER_QUERY);
			wire.SetExtraData("security", request.m_strSecurity);
			break;
		case Operation::Trades:
			wire.SetType(_TyRequest::TRADE_TRADE_QUERY);
			wire.SetExtraData("security", request.m_strSecurity);
			break;
		case Operation::Synchronize:
			wire.SetType(_TyRequest::TRADE_SYNC_QUERY);
			wire.SetExtraData("after_sequence", std::to_string(request.m_afterSequence));
			break;
		}
		return wire;
	}
	bool ReadReply(const ::CRequest& response, CReply& reply)
	{
		reply.m_requestId = response.GetId();
		std::optional<std::pair<int, std::string>> error = response.GetErrorInfo();
		if (error.has_value())
		{
			reply.m_result = { static_cast<Error>(error->first), error->second };
			return true;
		}
		const request::RequestData& data = response.GetData();
		utility::to_number(response.GetExtraData("version"), reply.m_snapshot.m_version);
		utility::to_number(response.GetExtraData("server_time_ms"), reply.m_snapshot.m_nTime);
		if (_TyRequest::TRADE_SYNC_QUERY == response.GetType())
		{
			if (!data.has_trade_sync())
			{
				return false;
			}
			const request::TradeSyncResult& sync = data.trade_sync();
			reply.m_snapshot.m_bReady = sync.ready();
			reply.m_snapshot.m_version = sync.watermark();
			reply.m_snapshot.m_nTime = sync.server_time_ms();
			for (const auto& row : sync.accounts())
			{
				reply.m_snapshot.m_accounts.push_back(ReadFunds(row));
			}
			for (const auto& row : sync.orders())
			{
				reply.m_snapshot.m_orders.push_back(ReadOrder(row));
			}
			for (const auto& row : sync.trades())
			{
				reply.m_snapshot.m_trades.push_back(ReadTrade(row));
			}
			for (const auto& row : sync.positions())
			{
				reply.m_snapshot.m_positions.push_back(ReadPosition(row));
			}
			for (const auto& row : sync.events())
			{
				CEvent event;
				if (!ReadEvent(row, event))
				{
					return false;
				}
				reply.m_events.push_back(std::move(event));
			}
			return true;
		}
		switch (response.GetType())
		{
		case _TyRequest::TRADE_ORDER_SUBMIT:
		case _TyRequest::TRADE_ORDER_CANCEL:
		case _TyRequest::TRADE_ORDER_QUERY:
			if (!data.has_trade_orders())
			{
				return false;
			}
			for (const auto& row : data.trade_orders().orders())
			{
				reply.m_snapshot.m_orders.push_back(ReadOrder(row));
			}
			if (_TyRequest::TRADE_ORDER_QUERY != response.GetType() && !reply.m_snapshot.m_orders.empty())
			{
				reply.m_orderId = reply.m_snapshot.m_orders.front().m_orderId;
			}
			return true;
		case _TyRequest::TRADE_TRADE_QUERY:
			if (!data.has_trades())
			{
				return false;
			}
			for (const auto& row : data.trades().trades())
			{
				reply.m_snapshot.m_trades.push_back(ReadTrade(row));
			}
			return true;
		case _TyRequest::TRADE_POSITION_QUERY:
			if (!data.has_trade_positions())
			{
				return false;
			}
			for (const auto& row : data.trade_positions().positions())
			{
				reply.m_snapshot.m_positions.push_back(ReadPosition(row));
			}
			return true;
		case _TyRequest::TRADE_ACCOUNT_INFO_QUERY:
			if (!data.has_account_info_list())
			{
				return false;
			}
			for (const auto& row : data.account_info_list().accounts())
			{
				if (0 != row.error_code())
				{
					reply.m_result = { Error::Unavailable, row.error_message() };
				}
				reply.m_snapshot.m_accounts.push_back(ReadFunds(row));
			}
			return true;
		case _TyRequest::TRADE_ACCOUNT_LOGIN:
		case _TyRequest::TRADE_ACCOUNT_CONNECT:
		case _TyRequest::TRADE_ACCOUNT_DISCONNECT:
			if (!data.has_account())
			{
				return false;
			}
			reply.m_snapshot.m_accounts.push_back({ data.account().account_id(), 0, 0, request::ACCOUNT_CONNECTED == data.account().state(), true });
			return true;
		default:
			return false;
		}
	}
	bool ReadPush(const ::CRequest& response, CEvent& event)
	{
		if (!utility::to_number(response.GetExtraData("sequence"), event.m_sequence) || !utility::to_number(response.GetExtraData("account_id"), event.m_accountId) || 0 == event.m_accountId)
		{
			return false;
		}
		utility::to_number(response.GetExtraData("request_id"), event.m_requestId);
		event.m_account.m_accountId = event.m_accountId;
		const request::RequestData& data = response.GetData();
		switch (response.GetType())
		{
		case _TyRequest::TRADE_ACCOUNT_STATE_PUSH:
			if (!data.has_account())
			{
				return false;
			}
			event.m_kind = EventKind::Session;
			event.m_account.m_bConnected = request::ACCOUNT_CONNECTED == data.account().state();
			return true;
		case _TyRequest::TRADE_ORDER_PUSH:
			if (!data.has_trade_orders() || 1 != data.trade_orders().orders_size())
			{
				return false;
			}
			event.m_kind = EventKind::Order;
			event.m_order = ReadOrder(data.trade_orders().orders(0));
			return true;
		case _TyRequest::TRADE_TRADE_PUSH:
			if (!data.has_trades() || 1 != data.trades().trades_size())
			{
				return false;
			}
			event.m_kind = EventKind::Trade;
			event.m_trade = ReadTrade(data.trades().trades(0));
			return true;
		case _TyRequest::TRADE_ACCOUNT_INFO_PUSH:
			if (!data.has_account_info_list() || 1 != data.account_info_list().accounts_size())
			{
				return false;
			}
			event.m_kind = EventKind::Funds;
			event.m_account = ReadFunds(data.account_info_list().accounts(0));
			return true;
		case _TyRequest::TRADE_POSITION_PUSH:
			if (!data.has_trade_positions() || 1 != data.trade_positions().positions_size())
			{
				return false;
			}
			event.m_kind = EventKind::Position;
			event.m_position = ReadPosition(data.trade_positions().positions(0));
			return true;
		default:
			return false;
		}
	}
} // namespace trade::protocol
