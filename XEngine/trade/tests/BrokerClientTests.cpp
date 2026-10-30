#include "CBrokerClient.h"
#include "TradeProtocol.h"
#include <iostream>
#include <stdexcept>
static _TyAccount TestAccount(std::uint64_t number)
{
	return 0 == number ? _TyAccount{} : _TyAccount{ 1, std::to_string(number) };
}

class CBrokerClientTests
{
	static void Check(bool bCondition, const char* strMessage)
	{
		if (!bCondition)
		{
			throw std::runtime_error(strMessage);
		}
	}
	static trade::CCounterReply Trades(std::uint64_t nVersion)
	{
		trade::CCounterReply reply;
		reply.m_accountIndex = TestAccount(1);
		reply.m_operation = trade::Operation::Trades;
		reply.m_version = nVersion;
		reply.m_data = std::vector<trade::CTrade>{};
		return reply;
	}

	static void Prepare(CBrokerClient& client, std::uint64_t nCycle)
	{
		CBrokerClient::CSync& pending = client.m_syncing[TestAccount(1)];
		pending.m_cycle = nCycle;
		pending.m_completed = 3;
		pending.m_data.m_account = { TestAccount(1), 100, 0, true, true };
		pending.m_data.m_version = 18;
		pending.m_versions = { 10, 14, 18, 0, 10 };
	}

	static CRequest FundsPush(std::uint64_t nSequence, std::int64_t nBalance)
	{
		trade::CCountEvent event;
		event.m_kind = trade::EventTy::Funds;
		event.m_accountIndex = TestAccount(1);
		event.m_sequence = nSequence;
		event.m_account = { TestAccount(1), nBalance, 0, true, true };
		return trade::protocol::Push(event, {});
	}

public:
	static void Run()
	{
		CBrokerClient client(CLoginInfo{});
		int nFunds = 0;
		client.SetEventHandler([&nFunds](const trade::CCountEvent& event)
		{
			if (trade::EventTy::Funds == event.m_kind)
			{
				++nFunds;
			}
		});
		Prepare(client, 1);
		client.QueryAccount(TestAccount(1));
		Check(1 == client.m_syncing.at(TestAccount(1)).m_cycle, "same account query sequence serialized");
		client.OnResponse(FundsPush(9, 90));
		client.OnResponse(FundsPush(12, 120));
		client.OnResponse(FundsPush(12, 999));
		client.CompleteQuery(Trades(20), 0, 1);
		Check(client.Snapshot(TestAccount(1)).m_bReady && 120 == client.Snapshot(TestAccount(1)).m_account.m_nBalance && 20 == client.Snapshot(TestAccount(1)).m_version && 1 == nFunds, "funds use their own watermark even below the trades watermark");
		client.OnResponse(FundsPush(25, 150));
		Check(150 == client.Snapshot(TestAccount(1)).m_account.m_nBalance && 2 == nFunds, "live push continues after queries");
		trade::CCounterReply query;
		query.m_accountIndex = TestAccount(1);
		query.m_operation = trade::Operation::Funds;
		query.m_version = 100;
		query.m_data = trade::CAccount{ TestAccount(1), 999, 0, true, true };
		CRequest response;
		response.SetType(_TyRequest::TRADE_FUNDS_QUERY);
		trade::protocol::WriteReply(query, response);
		client.CompleteRequest(response, TestAccount(1), 2, trade::Operation::Funds, 0, {});
		Check(25 == client.Snapshot(TestAccount(1)).m_version && 150 == client.Snapshot(TestAccount(1)).m_account.m_nBalance, "ordinary query never replaces account cache");
		client.OnState(SessionState::Disconnected);
		Prepare(client, 2);
		client.CompleteQuery(Trades(200), 0, 1);
		Check(!client.Snapshot(TestAccount(1)).m_bReady && client.m_syncing.contains(TestAccount(1)), "old connection cannot replace new queries");
		client.CompleteQuery(Trades(200), client.m_generation, 1);
		Check(!client.Snapshot(TestAccount(1)).m_bReady && client.m_syncing.contains(TestAccount(1)), "old query cycle cannot replace new queries");
		client.CompleteQuery(Trades(30), client.m_generation, 2);
		Check(client.Snapshot(TestAccount(1)).m_bReady && 30 == client.Snapshot(TestAccount(1)).m_version, "fresh reconnect restores all categories");
		client.OnState(SessionState::Disconnected);
		Prepare(client, 3);
		client.m_syncing.at(TestAccount(1)).m_bOverflow = true;
		client.CompleteQuery(Trades(40), client.m_generation, 3);
		Check(!client.Snapshot(TestAccount(1)).m_bReady && !client.m_syncing.contains(TestAccount(1)), "buffer overflow leaves cache unavailable for retry");
		Prepare(client, 4);
		trade::CCounterReply failed = Trades(40);
		failed.m_result = { trade::Error::Timeout, "query failed" };
		client.CompleteQuery(failed, client.m_generation, 4);
		Check(!client.Snapshot(TestAccount(1)).m_bReady && !client.m_syncing.contains(TestAccount(1)), "failed category never marks cache ready");

		Check(!client.Snapshot(TestAccount(0)).m_bReady, "zero account cannot aggregate cache");
		std::shared_ptr<CBrokerClient> shared = std::make_shared<CBrokerClient>(CLoginInfo{});
		shared->m_snapshots[TestAccount(1)].m_account.m_nBalance = 100;
		shared->m_snapshots[TestAccount(2)].m_account.m_nBalance = 200;
		std::shared_ptr<trade::ITradeSession> first = shared->CreateSession(TestAccount(1));
		std::shared_ptr<trade::ITradeSession> second = shared->CreateSession(TestAccount(2));
		Check(100 == first->Snapshot().m_account.m_nBalance && 200 == second->Snapshot().m_account.m_nBalance, "broker sessions bind snapshots to accounts");
		int firstEvents = 0, secondEvents = 0, rejectedReplies = 0;
		first->SetEventHandler([&firstEvents](const trade::CCountEvent&)
		{
			++firstEvents;
		});
		second->SetEventHandler([&secondEvents](const trade::CCountEvent&)
		{
			++secondEvents;
		});
		trade::CCountEvent accountEvent;
		accountEvent.m_accountIndex = TestAccount(2);
		shared->PublishSession(accountEvent);
		Check(0 == firstEvents && 1 == secondEvents, "broker events reach only their account");
		trade::CReqParam mismatch;
		mismatch.m_accountIndex = TestAccount(2);
		Check(!first->SendRequest(mismatch, [&rejectedReplies](const trade::CCounterReply&)
		{
			++rejectedReplies;
		}).Ok() && 0 == rejectedReplies,
			  "broker session rejects account mismatch without callback");
		first->Close();
		std::shared_ptr<trade::ITradeSession> replacement = shared->CreateSession(TestAccount(1));
		Check(first != replacement && 100 == replacement->Snapshot().m_account.m_nBalance && 200 == second->Snapshot().m_account.m_nBalance, "closed broker session replaced without stopping shared client");
		accountEvent.m_accountIndex = TestAccount(1);
		shared->PublishSession(accountEvent);
		Check(0 == firstEvents, "closed broker session receives no new events");
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
