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
		std::cout << "identity protocol tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
