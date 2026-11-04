#include "../../system/CEngineCenter.h"
#include "../../strategy/CTradeService.h"
#include "TradeProtocol.h"
#include "../../strategy/CStrategyService.h"
#include "../../strategy/CStrategyContext.h"
#include <iostream>
#include <stdexcept>
static _TyAccount TestAccount(std::uint64_t number)
{
	return 0 == number ? _TyAccount{} : _TyAccount{ 1, std::to_string(number) };
}

class CBrokerServiceTests
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
		reply.m_account = TestAccount(1);
		reply.m_operation = trade::Operation::Trades;
		reply.m_version = nVersion;
		reply.m_data = std::vector<trade::CTrade>{};
		return reply;
	}

	static void Prepare(CTradeService& service, std::uint64_t nCycle)
	{
		service.m_pAccountService->m_accounts[TestAccount(1)].m_info.m_state = TradeAccountState::Connected;
		CTradeService::CSync& pending = service.m_syncing[TestAccount(1)];
		pending.m_cycle = nCycle;
		pending.m_completed = 3;
		pending.m_data.m_account = { TestAccount(1), 100, 0, true, true };
		pending.m_data.m_version = 18;
		pending.m_versions = { 10, 14, 18, 0, 10 };
	}

	static CRequest FundsPush(std::uint64_t nSequence, std::int64_t nBalance)
	{
		trade::CCounterEvent event;
		event.m_kind = trade::EventTy::Funds;
		event.m_account = TestAccount(1);
		event.m_sequence = nSequence;
		event.m_funds = { TestAccount(1), nBalance, 0, true, true };
		return trade::protocol::Push(event, {});
	}

	static CRequest StrategyRequest(CTradeService& service, _TyRequest type)
	{
		CRequest request;
		request.SetId(42);
		request.SetType(type);
		WriteAccountIdentity(TestAccount(1), request);
		request.SetExtraData("request_source", "Strategy");
		request.SetExtraData("broker_connect_id", std::to_string(service.m_generation));
		request.SetExtraData("strategy_id", "7");
		request.SetExtraData("strategy_instance_id", "11");
		request.SetExtraData("signal_id", "8");
		request.SetExtraData("client_order_id", "42");
		request.SetExtraData("security", "600000.SSE");
		request.SetExtraData("quantity", "100");
		return request;
	}

	static void TestRouting(CTradeService& service, CAccountService& accounts, CSession& session)
	{
		int replies = 0;
		trade::Error lastError = trade::Error::None;
		service.SetStrategyReplyHandler([&replies, &lastError](const CRequest& origin, const trade::CCounterReply& reply)
		{
			Check(42 == origin.GetId() && 42 == reply.m_requestId && TestAccount(1) == reply.m_account, "strategy reply restores original identity");
			++replies;
			lastError = reply.m_result.m_error;
		});
		CRequest origin = StrategyRequest(service, _TyRequest::TRADE_ORDER_SUBMIT);
		CRequest response;
		response.SetType(origin.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::Rejected), "rejected");
		std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
		service.m_pending.emplace(response.GetId(), CTradeService::CPending{ origin, deadline });
		CRequest mismatch = response;
		mismatch.SetType(_TyRequest::TRADE_FUNDS_QUERY);
		service.OnReply(mismatch);
		Check(0 == replies && 1 == service.m_pending.size(), "wrong type preserves pending request");
		service.OnReply(response);
		service.OnReply(response);
		Check(1 == replies && service.m_pending.empty() && trade::Error::Rejected == lastError, "reply completes exactly once");
		service.m_pending.emplace(response.GetId(), CTradeService::CPending{ origin, deadline });
		service.RefreshPending();
		service.OnReply(response);
		Check(2 == replies && trade::Error::UnknownResult == lastError, "timeout completes once and ignores late reply");
		service.m_pending.emplace(response.GetId(), CTradeService::CPending{ origin, deadline });
		service.FailPending();
		Check(3 == replies && service.m_pending.empty(), "disconnect drains pending requests");
		origin.SetExtraData("broker_connect_id", std::to_string(service.m_generation + 1));
		service.DispatchReply(origin, response);
		Check(trade::Error::UnknownResult == lastError, "old connection cannot report definite rejection");
		origin.SetExtraData("broker_connect_id", "invalid");
		Check(!service.DispatchReply(origin, response) && 4 == replies, "malformed route does not invoke strategy handler");
		origin.SetExtraData("request_source", "Client");
		service.DispatchReply(origin, response);
		Check(4 == replies, "client request with strategy fields never invokes strategy handler");
		Prepare(service, 77);
		CRequest sync = StrategyRequest(service, _TyRequest::TRADE_FUNDS_QUERY);
		sync.SetExtraData("request_source", "AccountSync");
		sync.SetExtraData("sync_cycle", "77");
		CRequest failedQuery;
		failedQuery.SetType(sync.GetType());
		failedQuery.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "query failed");
		Check(service.DispatchReply(sync, failedQuery) && !service.m_syncing.contains(TestAccount(1)) && 4 == replies, "sync failure clears its cycle without routing to strategy");
		Prepare(service, 78);
		service.DispatchReply(sync, failedQuery);
		Check(78 == service.m_syncing.at(TestAccount(1)).m_cycle, "old sync cycle cannot cancel new synchronization");
		service.m_syncing.clear();
		service.SetStrategyReplyHandler({});
		TestStrategyReplies(service, accounts);
		session.NotifyState(SessionState::Ready, "test ready");
		accounts.m_accounts[TestAccount(1)].m_info.m_state = TradeAccountState::Connected;
		service.m_snapshots[TestAccount(1)].m_bReady = true;
		service.m_snapshots[TestAccount(1)].m_account.m_account = TestAccount(1);
		service.m_snapshots[TestAccount(1)].m_account.m_bConnected = true;
		service.m_snapshots[TestAccount(1)].m_account.m_bFundsValid = true;
		service.SetStrategyReplyHandler([&replies, &lastError](const CRequest&, const trade::CCounterReply& reply)
		{
			++replies;
			lastError = reply.m_result.m_error;
		});
		CRequest wire = StrategyRequest(service, _TyRequest::TRADE_ORDER_SUBMIT);
		Check(service.SendStrategyRequest(std::move(wire)).Ok(), "write failure is completed through reply path");
		Check(5 == replies && trade::Error::UnknownResult == lastError && service.m_pending.empty(), "write failure completes exactly once without retry");
		CRequest spoof = StrategyRequest(service, _TyRequest::TRADE_FUNDS_QUERY);
		service.OnRequest(spoof);
		Check(5 == replies && service.m_pending.empty(), "client cannot spoof strategy routing source");
		service.SetStrategyReplyHandler({});
		session.ClearHandlers();
	}

	static void TestStrategyReplies(CTradeService& service, CAccountService& accounts)
	{
		CStrategyService strategies(nullptr, &service, &accounts);
		std::shared_ptr<CStrategyRuntime> runtime = std::make_shared<CStrategyRuntime>();
		runtime->m_instanceId = 11;
		runtime->m_config.m_strategyId = 7;
		runtime->m_config.m_account = TestAccount(1);
		runtime->m_config.m_eventQueueLimit = 100;
		runtime->m_state.store(StrategyState::Stopped);
		runtime->m_bScheduled = true;
		strategies.m_runtimes.emplace(7, runtime);
		strategies.m_signals[{ 7, 8 }] = SignalState::Pending;
		struct CRuntimeGuard
		{
			std::unordered_map<_TyStrategyId, std::shared_ptr<CStrategyRuntime>>& m_runtimes;
			CStrategyRuntime& m_runtime;
			~CRuntimeGuard()
			{
				m_runtimes.clear();
				m_runtime.m_events.clear();
				m_runtime.m_bScheduled = false;
			}
		};
		CRuntimeGuard guard{ strategies.m_runtimes, *runtime };
		CRequest origin = StrategyRequest(service, _TyRequest::TRADE_ORDER_SUBMIT);
		trade::CCounterReply reply;
		reply.m_account = TestAccount(1);
		reply.m_operation = trade::Operation::Submit;
		strategies.OnReply(origin, reply);
		Check(runtime->m_events.empty(), "success waits for real order push");
		reply.m_result = { trade::Error::UnknownResult, "uncertain" };
		strategies.OnReply(origin, reply);
		Check(runtime->m_events.empty() && SignalState::Pending == strategies.m_signals.at({ 7, 8 }), "uncertain result preserves signal deduplication");
		reply.m_result = { trade::Error::Rejected, "rejected" };
		reply.m_operation = trade::Operation::Cancel;
		strategies.OnReply(origin, reply);
		Check(runtime->m_events.empty(), "cancel failure does not reject original order");
		reply.m_operation = trade::Operation::Submit;
		origin.SetExtraData("strategy_instance_id", "10");
		strategies.OnReply(origin, reply);
		Check(runtime->m_events.empty(), "old strategy instance reply is ignored");
		origin.SetExtraData("strategy_instance_id", "11");
		reply.m_account = TestAccount(2);
		strategies.OnReply(origin, reply);
		Check(runtime->m_events.empty(), "wrong account reply is ignored");
		reply.m_account = TestAccount(1);
		strategies.OnReply(origin, reply);
		Check(1 == runtime->m_events.size() && SignalState::Rejected == strategies.m_signals.at({ 7, 8 }) && StrategyState::Stopped == runtime->m_state.load(), "definite rejection queues existing order event without starting strategy");
		strategies.m_runtimes.clear();
		strategies.OnReply(origin, reply);
		Check(1 == runtime->m_events.size(), "deleted strategy reply is ignored");
		runtime->m_events.clear();
		runtime->m_bScheduled = false;
	}

public:
	static void Run()
	{
		int nFunds = 0;
		CSession session(CLoginInfo{});
		CAccountService accounts(&session);
		CTradeService service(&accounts, &session);
		service.m_bRunning.store(true);
		CAccountInfo info = FmtAccountInfo(TestAccount(1));
		info.m_state = TradeAccountState::Connected;
		accounts.m_accounts[TestAccount(1)].m_info = info;
		accounts.SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, &service));
		CEngineCenter center(nullptr, nullptr, &session, nullptr, nullptr, &service, &accounts);
		session.RegisterHandler(std::bind_front(&CEngineCenter::OnBrokerReply, &center));
		session.RegisterStateHandler(std::bind_front(&CEngineCenter::OnBrokerStateChanged, &center));
		service.SetEventHandler([&nFunds](const trade::CCounterEvent& event)
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
		session.Dispatch(FundsPush(9, 90));
		session.Dispatch(FundsPush(12, 120));
		session.Dispatch(FundsPush(12, 999));
		service.CompleteQuery(Trades(20), 0, 1);
		Check(service.Snapshot(TestAccount(1)).m_bReady && 120 == service.Snapshot(TestAccount(1)).m_account.m_nBalance && 20 == service.Snapshot(TestAccount(1)).m_version && 1 == nFunds, "funds use their own watermark even below the trades watermark");
		session.Dispatch(FundsPush(25, 150));
		Check(150 == service.Snapshot(TestAccount(1)).m_account.m_nBalance && 2 == nFunds, "live push continues after queries");
		trade::CCounterReply query;
		query.m_account = TestAccount(1);
		query.m_operation = trade::Operation::Funds;
		query.m_version = 100;
		query.m_data = trade::CFunds{ TestAccount(1), 999, 0, true, true };
		CRequest response;
		response.SetType(_TyRequest::TRADE_FUNDS_QUERY);
		trade::protocol::WriteReply(query, response);
		CRequest ordinaryQuery;
		ordinaryQuery.SetType(_TyRequest::TRADE_FUNDS_QUERY);
		ordinaryQuery.SetExtraData("request_source", "Client");
		service.DispatchReply(ordinaryQuery, response);
		Check(25 == service.Snapshot(TestAccount(1)).m_version && 150 == service.Snapshot(TestAccount(1)).m_account.m_nBalance, "ordinary query never replaces account cache");
		session.NotifyState(SessionState::Disconnected, "test disconnect");
		Check(!accounts.IsConnected(TestAccount(1)) && !service.IsReady(TestAccount(1)), "disconnect invalidates account connectivity and trade readiness");
		Prepare(service, 2);
		service.CompleteQuery(Trades(200), 0, 1);
		Check(!service.Snapshot(TestAccount(1)).m_bReady && service.m_syncing.contains(TestAccount(1)), "old connection cannot replace new queries");
		service.CompleteQuery(Trades(200), service.m_generation, 1);
		Check(!service.Snapshot(TestAccount(1)).m_bReady && service.m_syncing.contains(TestAccount(1)), "old query cycle cannot replace new queries");
		service.CompleteQuery(Trades(30), service.m_generation, 2);
		Check(service.Snapshot(TestAccount(1)).m_bReady && 30 == service.Snapshot(TestAccount(1)).m_version, "fresh reconnect restores all categories");
		session.NotifyState(SessionState::Disconnected, "test disconnect");
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
		accounts.OnAccountListReply(catalogue, accounts.m_broker_connect_id - 1);
		Check(accounts.HasAccount(TestAccount(1)) && service.m_syncing.contains(TestAccount(1)), "stale catalogue cannot remove current accounts");
		accounts.OnAccountListReply(catalogue, accounts.m_broker_connect_id);
		service.RefreshTrades();
		Check(!accounts.HasAccount(TestAccount(1)) && service.m_snapshots.empty() && service.m_syncing.empty(), "removed accounts discard snapshots and pending synchronization");
		service.CompleteQuery(Trades(50), service.m_generation, 5);
		Check(service.m_snapshots.empty(), "late query cannot recreate a removed account");

		TestRouting(service, accounts, session);
	}
};
int main()
{
	try
	{
		CBrokerServiceTests::Run();
		std::cout << "broker service tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
