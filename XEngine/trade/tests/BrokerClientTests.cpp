#include "../../strategy/CTradeService.h"
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

	static void Prepare(CTradeService& service, std::uint64_t nCycle)
	{
		service.m_pAccountService->m_accounts[TestAccount(1)].m_state = TradeAccountState::Connected;
		CTradeService::CSync& pending = service.m_syncing[TestAccount(1)];
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
		int nFunds = 0;
		CBrokerClient client(CLoginInfo{});
		CAccountService accounts(&client);
		CTradeService service(&accounts, &client);
		service.m_bRunning.store(true);
		CAccountInfo info = AccountInfo(TestAccount(1));
		info.m_state = TradeAccountState::Connected;
		accounts.m_accounts[TestAccount(1)] = info;
		accounts.SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, &service));
		client.SetEventHandler([&accounts, &service](const trade::CCountEvent& event)
		{
			accounts.OnBrokerEvent(event);
			service.OnBrokerEvent(event);
		});
		client.SetStateHandler([&accounts, &service](SessionState state)
		{
			accounts.OnBrokerState(state);
			service.OnBrokerState(state);
		});
		service.SetEventHandler([&nFunds](const trade::CCountEvent& event)
		{
			if (trade::EventTy::Funds == event.m_kind)
			{
				++nFunds;
			}
		});
		Check(accounts.IsConnected(TestAccount(1)) && !service.IsReady(TestAccount(1)), "connected account is not ready before financial synchronization");
		Prepare(service, 1);
		service.QueryAccount(TestAccount(1));
		Check(1 == service.m_syncing.at(TestAccount(1)).m_cycle, "same account query sequence serialized");
		client.OnResponse(FundsPush(9, 90));
		client.OnResponse(FundsPush(12, 120));
		client.OnResponse(FundsPush(12, 999));
		service.CompleteQuery(Trades(20), 0, 1);
		Check(service.Snapshot(TestAccount(1)).m_bReady && 120 == service.Snapshot(TestAccount(1)).m_account.m_nBalance && 20 == service.Snapshot(TestAccount(1)).m_version && 1 == nFunds, "funds use their own watermark even below the trades watermark");
		client.OnResponse(FundsPush(25, 150));
		Check(150 == service.Snapshot(TestAccount(1)).m_account.m_nBalance && 2 == nFunds, "live push continues after queries");
		trade::CCounterReply query;
		query.m_accountIndex = TestAccount(1);
		query.m_operation = trade::Operation::Funds;
		query.m_version = 100;
		query.m_data = trade::CAccount{ TestAccount(1), 999, 0, true, true };
		CRequest response;
		response.SetType(_TyRequest::TRADE_FUNDS_QUERY);
		trade::protocol::WriteReply(query, response);
		client.CompleteRequest(response, TestAccount(1), 2, trade::Operation::Funds, 0, {});
		Check(25 == service.Snapshot(TestAccount(1)).m_version && 150 == service.Snapshot(TestAccount(1)).m_account.m_nBalance, "ordinary query never replaces account cache");
		client.OnState(SessionState::Disconnected);
		Check(!accounts.IsConnected(TestAccount(1)) && !service.IsReady(TestAccount(1)), "disconnect invalidates account connectivity and trade readiness");
		Prepare(service, 2);
		service.CompleteQuery(Trades(200), 0, 1);
		Check(!service.Snapshot(TestAccount(1)).m_bReady && service.m_syncing.contains(TestAccount(1)), "old connection cannot replace new queries");
		service.CompleteQuery(Trades(200), service.m_generation, 1);
		Check(!service.Snapshot(TestAccount(1)).m_bReady && service.m_syncing.contains(TestAccount(1)), "old query cycle cannot replace new queries");
		service.CompleteQuery(Trades(30), service.m_generation, 2);
		Check(service.Snapshot(TestAccount(1)).m_bReady && 30 == service.Snapshot(TestAccount(1)).m_version, "fresh reconnect restores all categories");
		client.OnState(SessionState::Disconnected);
		Prepare(service, 3);
		service.m_syncing.at(TestAccount(1)).m_bOverflow = true;
		service.CompleteQuery(Trades(40), service.m_generation, 3);
		Check(!service.Snapshot(TestAccount(1)).m_bReady && !service.m_syncing.contains(TestAccount(1)), "buffer overflow leaves cache unavailable for retry");
		Prepare(service, 4);
		trade::CCounterReply failed = Trades(40);
		failed.m_result = { trade::Error::Timeout, "query failed" };
		service.CompleteQuery(failed, service.m_generation, 4);
		Check(!service.Snapshot(TestAccount(1)).m_bReady && !service.m_syncing.contains(TestAccount(1)), "failed category never marks cache ready");

		Check(!service.Snapshot(TestAccount(0)).m_bReady, "zero account cannot aggregate cache");
		service.m_snapshots[TestAccount(1)].m_account.m_nBalance = 100;
		service.m_snapshots[TestAccount(2)].m_account.m_nBalance = 200;
		Check(100 == service.Snapshot(TestAccount(1)).m_account.m_nBalance && 200 == service.Snapshot(TestAccount(2)).m_account.m_nBalance, "trade service isolates account snapshots");
		CRequest catalogue;
		catalogue.SetData(request::TradeAccountList{});
		Prepare(service, 5);
		accounts.OnCatalogue(catalogue, accounts.m_generation - 1);
		Check(accounts.HasAccount(TestAccount(1)) && service.m_syncing.contains(TestAccount(1)), "stale catalogue cannot remove current accounts");
		accounts.OnCatalogue(catalogue, accounts.m_generation);
		service.RefreshTrades();
		Check(!accounts.HasAccount(TestAccount(1)) && service.m_snapshots.empty() && service.m_syncing.empty(), "removed accounts discard snapshots and pending synchronization");
		service.CompleteQuery(Trades(50), service.m_generation, 5);
		Check(service.m_snapshots.empty(), "late query cannot recreate a removed account");

		trade::CReqParam invalid;
		int rejectedReplies = 0;
		Check(!client.SendRequest(invalid, [&rejectedReplies](const trade::CCounterReply&)
		{
			++rejectedReplies;
		}).Ok() && 0 == rejectedReplies,
			  "invalid account rejected without callback");
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
