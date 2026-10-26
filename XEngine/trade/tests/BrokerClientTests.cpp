#include "CBrokerClient.h"
#include "TradeProtocol.h"
#include <iostream>
#include <stdexcept>
class CBrokerClientTests
{
	static void Check(bool bCondition, const char* strMessage)
	{
		if (!bCondition)
		{
			throw std::runtime_error(strMessage);
		}
	}
	static CRequest Sync(trade::Id nVersion, trade::Id nAccountId = 1)
	{
		trade::CAccountData data;
		data.m_account = { nAccountId, 100, 0, true, true };
		data.m_bReady = true;
		data.m_version = nVersion;
		data.m_nTime = 1000;
		trade::CCounterReply reply;
		reply.m_accountId = nAccountId;
		reply.m_operation = trade::Operation::Synchronize;
		reply.m_version = nVersion;
		reply.m_nTime = 1000;
		reply.m_data = data;
		CRequest response;
		response.SetType(_TyRequest::TRADE_SYNC_QUERY);
		trade::protocol::WriteReply(reply, response);
		return response;
	}
	static CRequest FundsPush(trade::Id nSequence, std::int64_t nBalance)
	{
		trade::CEvent event;
		event.m_kind = trade::EventKind::Funds;
		event.m_accountId = 1;
		event.m_sequence = nSequence;
		event.m_account = { 1, nBalance, 0, true, true };
		return trade::protocol::Push(event, {});
	}

	public:
	static void Run()
	{
		CBrokerClient client(CLoginInfo{});
		int nFunds = 0;
		client.SetEventHandler([&nFunds](const trade::CEvent& event)
		{
			if (trade::EventKind::Funds == event.m_kind)
			{
				++nFunds;
			}
		});
		client.m_syncing.try_emplace(1);
		Check(!client.Request(trade::CRequest{ trade::Operation::Synchronize, 0, 1 }, {}).Ok(), "same account synchronization serialized");
		client.OnResponse(FundsPush(9, 90));
		client.OnResponse(FundsPush(12, 120));
		client.OnResponse(FundsPush(12, 999));
		client.CompleteRequest(Sync(10), 1, 1, trade::Operation::Synchronize, 0, {});
		Check(client.Snapshot(1).m_bReady && 120 == client.Snapshot(1).m_account.m_nBalance && 12 == client.Snapshot(1).m_version && 1 == nFunds, "buffered events newer than watermark applied exactly once");
		client.OnResponse(FundsPush(15, 150));
		Check(150 == client.Snapshot(1).m_account.m_nBalance && 2 == nFunds, "global sequence gaps accepted");
		trade::CCounterReply query;
		query.m_accountId = 1;
		query.m_operation = trade::Operation::Funds;
		query.m_version = 100;
		query.m_data = trade::CAccount{ 1, 999, 0, true, true };
		CRequest response;
		response.SetType(_TyRequest::TRADE_ACCOUNT_INFO_QUERY);
		trade::protocol::WriteReply(query, response);
		client.CompleteRequest(response, 1, 2, trade::Operation::Funds, 0, {});
		Check(15 == client.Snapshot(1).m_version && 150 == client.Snapshot(1).m_account.m_nBalance, "ordinary query never changes full cache");
		client.OnState(SessionState::Disconnected);
		client.m_syncing.try_emplace(1);
		client.CompleteRequest(Sync(200), 1, 3, trade::Operation::Synchronize, 0, {});
		Check(!client.Snapshot(1).m_bReady && client.m_syncing.contains(1), "old connection cannot replace data or new synchronization");
		client.CompleteRequest(Sync(20), 1, 4, trade::Operation::Synchronize, client.m_generation, {});
		Check(client.Snapshot(1).m_bReady && 20 == client.Snapshot(1).m_version, "fresh reconnect replaces complete data");
		client.m_syncing.try_emplace(1);
		client.m_syncing.at(1).m_bOverflow = true;
		client.CompleteRequest(Sync(30), 1, 5, trade::Operation::Synchronize, client.m_generation, {});
		Check(!client.Snapshot(1).m_bReady && !client.m_syncing.contains(1), "buffer overflow invalidates cache for retry");
		Check(!client.Snapshot(0).m_bReady, "zero account cannot aggregate cache");
	}
};
int main()
{
	try
	{
		CBrokerClientTests::Run();
		std::cout << "broker client tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
