#include "TradeProtocol.h"
#include <type_traits>
#include "../common/utility.h"
namespace trade::protocol
{
	void WriteOrder(const trade::COrder& row, request::TradeOrderRecord& wire)
	{
		SetAccountIdentity(row.m_accountIndex, wire);
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
		wire.set_broker_order_id(row.m_strBrokerOrderId);
		wire.set_trading_day(row.m_nDay);
		wire.set_raw_status(row.m_strRawStatus);
		wire.set_broker_error(row.m_strError);
		wire.set_cancel_pending(row.m_bCancelPending);
		wire.set_extended_fields(true);
		wire.set_fee_known(row.m_bFeeKnown);
		wire.set_frozen_known(row.m_bFrozenKnown);
		wire.set_sdk_client_id(row.m_sdkClientId);
		wire.set_broker_time(row.m_nBrokerTime);
	}
	void WriteTrade(const trade::CTrade& row, request::TradeRecord& wire)
	{
		SetAccountIdentity(row.m_accountIndex, wire);
		wire.set_trade_id(row.m_tradeId);
		wire.set_order_id(row.m_orderId);
		wire.set_security(row.m_strSecurity);
		wire.set_side(static_cast<int>(row.m_side));
		wire.set_price(row.m_nPrice);
		wire.set_price_scale(row.m_nPriceScale);
		wire.set_quantity(row.m_nQuantity);
		wire.set_fee(row.m_nFee);
		wire.set_time_ms(row.m_nTime);
		wire.set_broker_trade_id(row.m_strBrokerTradeId);
		wire.set_broker_order_id(row.m_strBrokerOrderId);
		wire.set_trading_day(row.m_nDay);
		wire.set_report_index(row.m_reportIndex);
		wire.set_market(row.m_market);
		wire.set_extended_fields(true);
		wire.set_fee_known(row.m_bFeeKnown);
	}
	void WritePosition(const trade::CPosition& row, request::TradePositionRecord& wire)
	{
		SetAccountIdentity(row.m_accountIndex, wire);
		wire.set_security(row.m_strSecurity);
		wire.set_total_quantity(row.m_nTotal);
		wire.set_available_quantity(row.m_nAvailable);
		wire.set_frozen_quantity(row.m_nFrozen);
		wire.set_today_quantity(row.m_nToday);
		wire.set_book_cost_cents(row.m_nCost);
		wire.set_extended_fields(true);
		wire.set_frozen_known(row.m_bFrozenKnown);
		wire.set_cost_known(row.m_bCostKnown);
	}
	CSecurity Security(const std::string& strValue)
	{
		std::size_t nDot = strValue.rfind('.');
		return std::string::npos == nDot ? CSecurity{} : CSecurity(strValue.substr(0, nDot), ParseMarket(strValue.substr(nDot + 1)));
	}
	void WriteFunds(const CAccount& row, bool bReady, request::TradeAccountInfo& wire)
	{
		SetAccountIdentity(row.m_accountIndex, wire);
		wire.set_balance(row.m_nBalance);
		wire.set_frozen(row.m_nFrozen);
		wire.set_available(row.m_bExplicitAvailable ? row.m_nAvailable : row.m_nBalance - row.m_nFrozen);
		wire.set_extended_fields(true);
		wire.set_funds_valid(row.m_bFundsValid);
		wire.set_frozen_known(row.m_bFrozenKnown);
		wire.set_account_state(row.m_nState);
		wire.set_available_snapshot(bReady && row.m_bConnected && row.m_bFundsValid);
		wire.set_connected(row.m_bConnected);
	}

	void WriteReply(const CCounterReply& reply, ::CRequest& response)
	{
		request::TradeCounterReply wire;
		wire.set_request_id(reply.m_requestId);
		SetAccountIdentity(reply.m_accountIndex, wire);
		wire.set_operation(static_cast<int>(reply.m_operation));
		wire.set_error_code(static_cast<int>(reply.m_result.m_error));
		wire.set_error_message(reply.m_result.m_strMessage);
		wire.set_watermark(reply.m_version);
		wire.set_server_time_ms(reply.m_nTime);
		response.SetErrorInfo(static_cast<int>(reply.m_result.m_error), reply.m_result.m_strMessage);
		if (reply.m_result.Ok())
		{
			std::visit([&wire](const auto& value)
			{
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, COperationReceipt>)
				{
					wire.mutable_receipt()->set_order_id(value.m_orderId);
				}
				else if constexpr (std::is_same_v<T, CAccount>)
				{
					WriteFunds(value, true, *wire.mutable_funds());
					wire.mutable_funds()->set_server_time_ms(wire.server_time_ms());
				}
				else if constexpr (std::is_same_v<T, std::vector<CPosition>>)
				{
					request::TradePositionList* rows = wire.mutable_positions();
					for (const auto& row : value)
					{
						WritePosition(row, *rows->add_positions());
					}
				}
				else if constexpr (std::is_same_v<T, std::vector<COrder>>)
				{
					request::TradeOrderList* rows = wire.mutable_orders();
					for (const auto& row : value)
					{
						WriteOrder(row, *rows->add_orders());
					}
				}
				else if constexpr (std::is_same_v<T, std::vector<CTrade>>)
				{
					request::TradeList* rows = wire.mutable_trades();
					for (const auto& row : value)
					{
						WriteTrade(row, *rows->add_trades());
					}
				}
			}, reply.m_data);
		}
		response.SetData(wire);
	}
	void WriteEvent(const CCountEvent& event, request::TradeEventRecord& wire)
	{
		wire.set_sequence(event.m_sequence);
		wire.set_request_id(event.m_requestId);
		SetAccountIdentity(event.m_accountIndex, wire);
		wire.set_kind(static_cast<int>(event.m_kind));
		wire.set_connected(event.m_account.m_bConnected);
		wire.set_extended_fields(true);
		wire.set_account_valid(event.m_bAccountValid);
		if (event.m_bAccountValid)
		{
			WriteFunds(event.m_account, true, *wire.mutable_account_snapshot());
		}
		switch (event.m_kind)
		{
		case EventTy::Order:
			WriteOrder(event.m_order, *wire.mutable_order());
			break;
		case EventTy::Deal:
			WriteTrade(event.m_trade, *wire.mutable_trade());
			break;
		case EventTy::Position:
			WritePosition(event.m_position, *wire.mutable_position());
			break;
		case EventTy::Funds:
			WriteFunds(event.m_account, true, *wire.mutable_funds());
			break;
		default:
			break;
		}
	}
	COrderEvent OrderEvent(const CCountEvent& event)
	{
		const trade::COrder& row = event.m_order;
		COrderEvent order;
		order.m_sequence = event.m_sequence;
		order.m_accountIndex = event.m_accountIndex;
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
		case trade::Status::Pending:
			order.m_status = OrderStatus::Pending;
			break;
		case trade::Status::Unknown:
			order.m_status = OrderStatus::Unknown;
			break;
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
		if (row.m_bCancelPending && trade::IsActive(row.m_status))
		{
			order.m_status = OrderStatus::Cancelling;
		}
		return order;
	}
	CTradeEvent TradeEvent(const CCountEvent& event)
	{
		CTradeEvent trade;
		trade.m_sequence = event.m_sequence;
		trade.m_accountIndex = event.m_accountIndex;
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
		SetAccountIdentity(AccountIndex(info), payload);
		payload.set_site(info.m_strSite);
		payload.set_broker_name(info.m_strBrokerName);
		payload.set_name(info.m_strName);
		payload.set_state(static_cast<request::TradingAccountState>(info.m_state));
		payload.set_last_error(info.m_strLastError);
		return payload;
	}

	::CRequest Push(const CCountEvent& event, const CTradeAccountInfo& account)
	{
		::CRequest push;
		push.SetId(0);
		push.SetExtraData("sequence", std::to_string(event.m_sequence));
		push.SetExtraData("request_id", std::to_string(event.m_requestId));
		push.SetExtraData("broker_id", std::to_string(event.m_accountIndex.first));
		push.SetExtraData("account", event.m_accountIndex.second);
		switch (event.m_kind)
		{
		case trade::EventTy::Session:
			push.SetType(_TyRequest::TRADE_ACCOUNT_STATE_PUSH);
			push.SetData(AccountPayload(account));
			break;
		case trade::EventTy::Order:
		{
			push.SetType(_TyRequest::TRADE_ORDER_PUSH);
			request::TradeOrderList rows;
			WriteOrder(event.m_order, *rows.add_orders());
			push.SetData(rows);
		}
		break;
		case trade::EventTy::Deal:
		{
			push.SetType(_TyRequest::TRADE_TRADE_PUSH);
			request::TradeList rows;
			WriteTrade(event.m_trade, *rows.add_trades());
			push.SetData(rows);
		}
		break;
		case trade::EventTy::Funds:
		{
			push.SetType(_TyRequest::TRADE_ACCOUNT_INFO_PUSH);
			request::TradeAccountInfo rows;
			WriteFunds(event.m_account, true, rows);
			push.SetData(rows);
		}
		break;
		case trade::EventTy::Position:
		{
			push.SetType(_TyRequest::TRADE_POSITION_PUSH);
			request::TradePositionList rows;
			WritePosition(event.m_position, *rows.add_positions());
			push.SetData(rows);
		}
		break;
		}
		if (event.m_bAccountValid)
		{
			request::TradeAccountInfo funds;
			WriteFunds(event.m_account, true, funds);
			push.SetEventAccount(funds);
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
			row.m_accountIndex = AccountIndex(wire);
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
			row.m_strBrokerOrderId = wire.broker_order_id();
			row.m_nDay = wire.trading_day();
			row.m_strRawStatus = wire.raw_status();
			row.m_strError = wire.broker_error();
			row.m_bCancelPending = wire.cancel_pending();
			row.m_bFeeKnown = !wire.extended_fields() || wire.fee_known();
			row.m_bFrozenKnown = !wire.extended_fields() || wire.frozen_known();
			row.m_sdkClientId = wire.sdk_client_id();
			row.m_nBrokerTime = wire.broker_time();
			return row;
		}
		CTrade ReadTrade(const request::TradeRecord& wire)
		{
			CTrade row;
			row.m_accountIndex = AccountIndex(wire);
			row.m_orderId = wire.order_id();
			row.m_tradeId = wire.trade_id();
			row.m_strSecurity = wire.security();
			row.m_nPrice = wire.price();
			row.m_nPriceScale = wire.price_scale();
			row.m_nQuantity = wire.quantity();
			row.m_nFee = wire.fee();
			row.m_nTime = wire.time_ms();
			row.m_side = static_cast<Side>(wire.side());
			row.m_strBrokerTradeId = wire.broker_trade_id();
			row.m_strBrokerOrderId = wire.broker_order_id();
			row.m_nDay = wire.trading_day();
			row.m_reportIndex = wire.report_index();
			row.m_market = wire.market();
			row.m_bFeeKnown = !wire.extended_fields() || wire.fee_known();
			return row;
		}
		CPosition ReadPosition(const request::TradePositionRecord& wire)
		{
			CPosition row{ AccountIndex(wire), wire.security(), wire.total_quantity(), wire.available_quantity(), wire.frozen_quantity(), wire.today_quantity(), wire.book_cost_cents() };
			row.m_bCostKnown = !wire.extended_fields() || wire.cost_known();
			row.m_bFrozenKnown = !wire.extended_fields() || wire.frozen_known();
			return row;
		}
		CAccount ReadFunds(const request::TradeAccountInfo& wire)
		{
			CAccount row{ AccountIndex(wire), wire.balance(), wire.frozen(), wire.connected(), true };
			row.m_nAvailable = wire.available();
			row.m_bExplicitAvailable = true;
			row.m_bFundsValid = !wire.extended_fields() || wire.funds_valid();
			row.m_bFrozenKnown = !wire.extended_fields() || wire.frozen_known();
			row.m_nState = wire.account_state();
			return row;
		}
		bool ReadEvent(const request::TradeEventRecord& wire, CCountEvent& event)
		{
			if (0 > wire.kind() || static_cast<int>(EventTy::Position) < wire.kind())
			{
				return false;
			}
			event.m_sequence = wire.sequence();
			event.m_accountIndex = AccountIndex(wire);
			event.m_requestId = wire.request_id();
			event.m_kind = static_cast<EventTy>(wire.kind());
			event.m_account.m_accountIndex = event.m_accountIndex;
			event.m_account.m_bConnected = wire.connected();
			event.m_bAccountValid = !wire.extended_fields() || wire.account_valid();
			if (wire.has_account_snapshot())
			{
				event.m_account = ReadFunds(wire.account_snapshot());
			}
			switch (event.m_kind)
			{
			case EventTy::Order:
				if (!wire.has_order())
				{
					return false;
				}
				event.m_order = ReadOrder(wire.order());
				break;
			case EventTy::Deal:
				if (!wire.has_trade())
				{
					return false;
				}
				event.m_trade = ReadTrade(wire.trade());
				break;
			case EventTy::Funds:
				if (!wire.has_funds())
				{
					return false;
				}
				event.m_account = ReadFunds(wire.funds());
				break;
			case EventTy::Position:
				if (!wire.has_position())
				{
					return false;
				}
				event.m_position = ReadPosition(wire.position());
				break;
			case EventTy::Session:
				break;
			}
			return true;
		}
	} // namespace
	::CRequest WriteRequest(const CReqParam& request)
	{
		::CRequest wire;
		wire.SetId(request.m_requestId);
		wire.SetExtraData("broker_id", std::to_string(request.m_accountIndex.first));
		wire.SetExtraData("account", request.m_accountIndex.second);
		switch (request.GetOperation())
		{
		case Operation::Login:
		{
			const CLoginParam& params = std::get<CLoginParam>(request.m_param);
			wire.SetType(_TyRequest::TRADE_ACCOUNT_LOGIN);
			wire.SetExtraData("password", params.m_strPassword);
			wire.SetExtraData("site", params.m_strSite);
			break;
		}
		case Operation::Connect:
			wire.SetType(_TyRequest::UNKNOWN);
			break;
		case Operation::Disconnect:
			wire.SetType(_TyRequest::TRADE_ACCOUNT_DISCONNECT);
			break;
		case Operation::Submit:
		{
			const CSubmitParam& params = std::get<CSubmitParam>(request.m_param);
			wire.SetType(_TyRequest::TRADE_ORDER_SUBMIT);
			wire.SetExtraData("client_order_id", std::to_string(params.m_order.m_clientId));
			wire.SetExtraData("security", params.m_order.m_strSecurity);
			wire.SetExtraData("side", Side::Buy == params.m_order.m_side ? "buy" : "sell");
			wire.SetExtraData("order_type", "limit");
			wire.SetExtraData("price", std::to_string(params.m_order.m_nPrice));
			wire.SetExtraData("price_scale", std::to_string(params.m_order.m_nPriceScale));
			wire.SetExtraData("quantity", std::to_string(params.m_order.m_nQuantity));
			wire.SetExtraData("strategy_id", std::to_string(params.m_order.m_strategyId));
			wire.SetExtraData("signal_id", std::to_string(params.m_order.m_signalId));
			break;
		}
		case Operation::Cancel:
			wire.SetType(_TyRequest::TRADE_ORDER_CANCEL);
			wire.SetExtraData("order_id", std::to_string(std::get<trade::CCancelParam>(request.m_param).m_orderId));
			break;
		case Operation::Funds:
			wire.SetType(_TyRequest::TRADE_FUNDS_QUERY);
			break;
		case Operation::Positions:
			wire.SetType(_TyRequest::TRADE_POSITION_QUERY);
			break;
		case Operation::Orders:
			wire.SetType(_TyRequest::TRADE_ORDER_QUERY);
			break;
		case Operation::Trades:
			wire.SetType(_TyRequest::TRADE_DEAL_QUERY);
			break;
		}
		return wire;
	}
	bool ReadReply(const ::CRequest& response, CCounterReply& reply)
	{
		reply = CCounterReply{};
		reply.m_requestId = response.GetId();
		std::optional<std::pair<int, std::string>> error = response.GetErrorInfo();
		if (error.has_value())
		{
			reply.m_result = { static_cast<Error>(error->first), error->second };
			return true;
		}
		if (!response.GetData().has_counter_reply())
		{
			return false;
		}
		const request::TradeCounterReply& wire = response.GetData().counter_reply();
		if (!IsAccountValid(AccountIndex(wire)) || static_cast<int>(Operation::Login) > wire.operation() || static_cast<int>(Operation::Trades) < wire.operation())
		{
			return false;
		}
		reply.m_accountIndex = AccountIndex(wire);
		reply.m_requestId = wire.request_id();
		reply.m_operation = static_cast<Operation>(wire.operation());
		reply.m_version = wire.watermark();
		reply.m_nTime = wire.server_time_ms();
		reply.m_result = { static_cast<Error>(wire.error_code()), wire.error_message() };
		if (!reply.m_result.Ok())
		{
			return request::TradeCounterReply::DATA_NOT_SET == wire.data_case();
		}
		switch (reply.m_operation)
		{
		case Operation::Funds:
			if (!wire.has_funds() || reply.m_accountIndex != AccountIndex(wire.funds()))
			{
				return false;
			}
			reply.m_data = ReadFunds(wire.funds());
			return true;
		case Operation::Positions:
		{
			if (!wire.has_positions())
			{
				return false;
			}
			std::vector<CPosition> rows;
			for (const auto& row : wire.positions().positions())
			{
				if (reply.m_accountIndex != AccountIndex(row))
				{
					return false;
				}
				rows.emplace_back(ReadPosition(row));
			}
			reply.m_data = std::move(rows);
			return true;
		}
		case Operation::Orders:
		{
			if (!wire.has_orders())
			{
				return false;
			}
			std::vector<COrder> rows;
			for (const auto& row : wire.orders().orders())
			{
				if (reply.m_accountIndex != AccountIndex(row))
				{
					return false;
				}
				rows.emplace_back(ReadOrder(row));
			}
			reply.m_data = std::move(rows);
			return true;
		}
		case Operation::Trades:
		{
			if (!wire.has_trades())
			{
				return false;
			}
			std::vector<CTrade> rows;
			for (const auto& row : wire.trades().trades())
			{
				if (reply.m_accountIndex != AccountIndex(row))
				{
					return false;
				}
				rows.emplace_back(ReadTrade(row));
			}
			reply.m_data = std::move(rows);
			return true;
		}
		default:
			if (!wire.has_receipt())
			{
				return false;
			}
			reply.m_data = COperationReceipt{ wire.receipt().order_id() };
			return true;
		}
	}
	bool ReadPush(const ::CRequest& response, CCountEvent& event)
	{
		if (!utility::to_number(response.GetExtraData("sequence"), event.m_sequence) || !ReadAccountIdentity(response, event.m_accountIndex) || !IsAccountValid(event.m_accountIndex))
		{
			return false;
		}
		utility::to_number(response.GetExtraData("request_id"), event.m_requestId);
		event.m_account.m_accountIndex = event.m_accountIndex;
		const request::RequestData& data = response.GetData();
		event.m_bAccountValid = data.has_event_account();
		if (data.has_event_account())
		{
			event.m_account = ReadFunds(data.event_account());
		}
		switch (response.GetType())
		{
		case _TyRequest::TRADE_ACCOUNT_STATE_PUSH:
			if (!data.has_account())
			{
				return false;
			}
			event.m_kind = EventTy::Session;
			event.m_account.m_bConnected = request::ACCOUNT_CONNECTED == data.account().state();
			event.m_account.m_nState = static_cast<int>(data.account().state());
			return event.m_accountIndex == AccountIndex(data.account());
		case _TyRequest::TRADE_ORDER_PUSH:
			if (!data.has_trade_orders() || 1 != data.trade_orders().orders_size())
			{
				return false;
			}
			event.m_kind = EventTy::Order;
			event.m_order = ReadOrder(data.trade_orders().orders(0));
			return event.m_accountIndex == event.m_order.m_accountIndex;
		case _TyRequest::TRADE_TRADE_PUSH:
			if (!data.has_trades() || 1 != data.trades().trades_size())
			{
				return false;
			}
			event.m_kind = EventTy::Deal;
			event.m_trade = ReadTrade(data.trades().trades(0));
			return event.m_accountIndex == event.m_trade.m_accountIndex;
		case _TyRequest::TRADE_ACCOUNT_INFO_PUSH:
			if (!data.has_account_info())
			{
				return false;
			}
			event.m_kind = EventTy::Funds;
			event.m_bAccountValid = true;
			event.m_account = ReadFunds(data.account_info());
			return event.m_accountIndex == event.m_account.m_accountIndex;
		case _TyRequest::TRADE_POSITION_PUSH:
			if (!data.has_trade_positions() || 1 != data.trade_positions().positions_size())
			{
				return false;
			}
			event.m_kind = EventTy::Position;
			event.m_position = ReadPosition(data.trade_positions().positions(0));
			return event.m_accountIndex == event.m_position.m_accountIndex;
		default:
			return false;
		}
	}
} // namespace trade::protocol
