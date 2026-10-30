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
		CTradeAccountInfo info = AccountInfo(account);
		info.m_strBrokerName = "测试券商";
		info.m_strName = "账户别名";
		Check(account == AccountIndex(trade::protocol::AccountPayload(info)), "display identity preserved");
		for (trade::Operation operation : { trade::Operation::Funds, trade::Operation::Positions, trade::Operation::Orders, trade::Operation::Trades })
		{
			trade::CCounterReply reply;
			reply.m_accountIndex = account;
			reply.m_operation = operation;
			switch (operation)
			{
			case trade::Operation::Funds:
				reply.m_data = trade::CAccount{ account, 1000, 10, true, true };
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
			Check(trade::protocol::ReadReply(response, decoded) && account == decoded.m_accountIndex, "category identity round trip");
		}
		trade::CReqParam login;
		login.m_accountIndex = account;
		login.m_param = trade::CLoginParam{ "secret", "上海" };
		CRequest wire = trade::protocol::WriteRequest(login);
		_TyAccount parsed;
		Check(ReadAccountIdentity(wire, parsed) && parsed == account && "secret" == wire.GetExtraData("password") && "上海" == wire.GetExtraData("site"), "login credentials and identity");

		trade::CCountEvent event;
		event.m_accountIndex = account;
		event.m_sequence = 100;
		event.m_kind = trade::EventTy::Order;
		event.m_account.m_accountIndex = account;
		event.m_account.m_bConnected = true;
		event.m_account.m_nBalance = 10000;
		event.m_account.m_nFrozen = 100;
		event.m_account.m_nAvailable = 7777;
		event.m_account.m_bExplicitAvailable = true;
		event.m_account.m_bFundsValid = false;
		event.m_order.m_accountIndex = account;
		event.m_order.m_orderId = 99;
		event.m_order.m_strBrokerOrderId = "18446744073709551614";
		event.m_order.m_status = trade::Status::Unknown;
		event.m_order.m_bFeeKnown = false;
		event.m_order.m_bFrozenKnown = false;
		CRequest pushed = trade::protocol::Push(event, info);
		trade::CCountEvent received;
		Check(trade::protocol::ReadPush(pushed, received) && received.m_bAccountValid, "account metadata in order push");
		Check(7777 == received.m_account.m_nAvailable && !received.m_account.m_bFundsValid, "explicit funds and validity round trip");
		Check(event.m_order.m_strBrokerOrderId == received.m_order.m_strBrokerOrderId && !received.m_order.m_bFeeKnown, "raw identifier and unknown fee preserved");
		Check(OrderStatus::Unknown == trade::protocol::OrderEvent(received).m_status, "unknown is not rejected");
		received.m_order.m_status = trade::Status::Pending;
		Check(OrderStatus::Pending == trade::protocol::OrderEvent(received).m_status, "pending is not accepted");
		received.m_order.m_status = trade::Status::PartiallyFilled;
		received.m_order.m_bCancelPending = true;
		Check(OrderStatus::Cancelling == trade::protocol::OrderEvent(received).m_status, "pending cancel is distinct");
		trade::CAccountData cached;
		cached.m_account = event.m_account;
		received.m_bAccountValid = false;
		received.m_account = {};
		trade::UpdateAccountData(cached, received);
		Check(7777 == cached.m_account.m_nAvailable, "missing account metadata cannot overwrite funds");
		std::cout << "identity protocol tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
