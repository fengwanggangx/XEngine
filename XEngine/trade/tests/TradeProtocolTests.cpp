#include "TradeProtocol.h"
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message)
{
	if (!value)
	{
		throw std::runtime_error(message);
	}
}

int main()
{
	try
	{
		_TyAccount account{ 17, "000123" };
		CAccountInfo info = FmtAccountInfo(account);
		info.m_broker_name = "测试券商";
		info.m_name = "账户别名";
		Check(account == FmtAccount(trade::protocol::AccountPayload(info)), "display identity preserved");
		for (trade::Operation operation : { trade::Operation::Funds, trade::Operation::Positions, trade::Operation::Orders, trade::Operation::Trades })
		{
			trade::CCounterReply reply;
			reply.m_account = account;
			reply.m_operation = operation;
			switch (operation)
			{
			case trade::Operation::Funds:
				reply.m_data = trade::CFunds{ account, 1000, 10, true, true };
				break;
			case trade::Operation::Positions:
				reply.m_data = std::vector<trade::CPosition>{};
				break;
			case trade::Operation::Orders:
				reply.m_data = std::vector<trade::COrder>{};
				break;
			case trade::Operation::Trades:
				reply.m_data = std::vector<trade::CTrade>{};
				break;
			default:
				break;
			}
			CRequest response;
			trade::protocol::WriteReply(reply, response);
			trade::CCounterReply decoded;
			Check(trade::protocol::ReadReply(response, decoded) && account == decoded.m_account, "category identity round trip");
		}
		trade::CReqParam login;
		login.m_account = account;
		login.m_param = trade::CLoginParam{ "secret", "上海" };
		CRequest wire = trade::protocol::WriteRequest(login);
		_TyAccount parsed;
		Check(FmtAccount(wire, parsed) && parsed == account && "secret" == wire.GetExtraData("password") && "上海" == wire.GetExtraData("site"), "login credentials and identity");

		trade::CCounterEvent event;
		event.m_account = account;
		event.m_sequence = 100;
		event.m_kind = trade::EventTy::Order;
		event.m_funds.m_account = account;
		event.m_funds.m_connected = true;
		event.m_funds.m_balance = 10000;
		event.m_funds.m_frozen = 100;
		event.m_funds.m_available = 7777;
		event.m_funds.m_explicit_available = true;
		event.m_funds.m_funds_valid = false;
		event.m_order.m_account = account;
		event.m_order.m_order_id = 99;
		event.m_order.m_broker_order_id = "18446744073709551614";
		event.m_order.m_status = trade::Status::Unknown;
		event.m_order.m_fee_known = false;
		event.m_order.m_frozen_known = false;
		CRequest pushed = trade::protocol::Push(event, info);
		trade::CCounterEvent received;
		Check(trade::protocol::ReadPush(pushed, received) && received.m_account_valid, "account metadata in order push");
		Check(7777 == received.m_funds.m_available && !received.m_funds.m_funds_valid, "explicit funds and validity round trip");
		Check(event.m_order.m_broker_order_id == received.m_order.m_broker_order_id && !received.m_order.m_fee_known, "raw identifier and unknown fee preserved");
		Check(OrderStatus::Unknown == trade::protocol::OrderEvent(received).m_status, "unknown is not rejected");
		received.m_order.m_status = trade::Status::Pending;
		Check(OrderStatus::Pending == trade::protocol::OrderEvent(received).m_status, "pending is not accepted");
		received.m_order.m_status = trade::Status::PartiallyFilled;
		received.m_order.m_cancel_pending = true;
		Check(OrderStatus::Cancelling == trade::protocol::OrderEvent(received).m_status, "pending cancel is distinct");
		trade::CAccountData cached;
		cached.m_account = event.m_funds;
		received.m_account_valid = false;
		received.m_funds = {};
		trade::UpdateAccountData(cached, received);
		Check(7777 == cached.m_account.m_available, "missing account metadata cannot overwrite funds");
		std::cout << "identity protocol tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
