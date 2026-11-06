#include "../CTradeService.h"
#include "../CStrategyService.h"
#include "../CStrategyContext.h"
#include "../../system/CEngineCenter.h"
#include "../../system/CHQService.h"
#include "../../trade/TradeProtocol.h"
#include "../../request/request.pb.h"
#include "../../network/CNetPool.h"
#include "../../network/CTcpServer.h"
#include "../../network/CFrameBuffer.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <iostream>
#include <stdexcept>

class CServiceRoutingTests
{
	static void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	static CRequest LastResponse(bufferevent* event)
	{
		evbuffer* output = bufferevent_get_output(event);
		std::string bytes(evbuffer_get_length(output), '\0');
		// Libevent freezes the output front; temporarily allow inspection without draining it.
		evbuffer_unfreeze(output, 1);
		ev_ssize_t copied = evbuffer_copyout(output, bytes.data(), bytes.size());
		evbuffer_freeze(output, 1);
		Check(0 < copied, "response must be sent to the original connection");
		net::CFrameBuffer decoder;
		std::optional<std::vector<std::string>> frames = decoder.Decode(bytes.data(), bytes.size());
		Check(frames.has_value() && !frames->empty(), "response frame decodes");
		CRequest response;
		Check(response.Deserialize(frames->back()), "response payload decodes");
		return response;
	}

	static void TestBrokerRouting(bufferevent* event, CEngineCenter& center, CAccountService& accounts, CTradeService& trader)
	{
		CRequest origin;
		origin.SetId(301);
		origin.SetConnectionId(7070);
		origin.SetType(_TyRequest::TRADE_ACCOUNT_LOGIN);
		std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
		accounts.m_pending.emplace(9101, CAccountService::CPending{ origin, deadline });
		CRequest response;
		response.SetId(9101);
		response.SetType(origin.GetType());
		response.SetErrorInfo(1007, "test rejection");
		Check(!center.OnHQMarketReply(response) && accounts.m_pending.contains(9101), "HQ response cannot consume Broker pending state");
		Check(center.OnBrokerReply(response) && accounts.m_pending.empty(), "Broker login response reaches account service");
		Check(301 == LastResponse(event).GetId(), "account response restores original client request id");
		Check(!center.OnBrokerReply(response), "duplicate account response is ignored");

		origin.SetId(302);
		origin.SetType(_TyRequest::TRADE_FUNDS_QUERY);
		origin.SetExtraData("request_source", "Client");
		trader.m_pending.emplace(9102, CTradeService::CPending{ origin, deadline });
		response.SetId(9102);
		response.SetType(_TyRequest::TRADE_ORDER_QUERY);
		Check(!center.OnBrokerReply(response) && trader.m_pending.contains(9102), "mismatched trade response preserves pending request");
		response.SetType(origin.GetType());
		Check(center.OnBrokerReply(response) && trader.m_pending.empty(), "Broker funds response reaches trade service");
		Check(302 == LastResponse(event).GetId(), "trade response restores original client request id");
		Check(!center.OnBrokerReply(response), "duplicate trade response is ignored");
	}

	static void TestMarketRouting(bufferevent* event, CEngineCenter& center, CHQService& market, CStrategyService& strategies)
	{
		CRequest query;
		query.SetId(201);
		query.SetConnectionId(7070);
		query.SetType(_TyRequest::MARKET_QUOTE_QUERY);
		query.SetExtraData("security", "600519.SSE");
		std::string key = market.GetQueryKey(query);
		CPendingQuery pending;
		pending.m_routerId = 9001;
		pending.m_strKey = key;
		pending.m_type = query.GetType();
		pending.m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		pending.m_clients.push_back({ 7070, 200 });
		market.m_pendingQueries.emplace(key, pending);
		center.OnRequest(query);
		Check(2 == market.m_pendingQueries.at(key).m_clients.size(), "identical client market queries merge");
		CRequest response;
		response.SetId(9001);
		response.SetType(query.GetType());
		Check(center.OnHQMarketReply(response), "HQ reply reaches the pending query owner");
		Check(201 == LastResponse(event).GetId() && market.m_pendingQueries.empty(), "market response restores client request identity and consumes pending state");
		Check(!center.OnBrokerReply(response), "wrong upstream cannot consume a market response");

		CQuoteInfo quote("600519", Exchange::sse, Channel::quote);
		CStrategyConfig cfg;
		cfg.m_strategyId = 42;
		cfg.m_subscriptions = { quote };
		strategies.AddRoutes(cfg);
		Check(market.HasDemand(quote), "strategy routes acquire shared HQ demand");
		strategies.RemoveRoutes(cfg);
		Check(!market.HasDemand(quote), "removing strategy routes releases HQ demand");
		market.m_subscriptions.Subscribe(FmtHQSubID(HQOrigin::Client, 7070), { quote });
		market.m_upstreamSubscriptions[quote.String()] = { quote, 9002, _TyRequest::MARKET_SUBSCRIBE, false, pending.m_deadline };
		market.m_pendingBatchSubscriptions.emplace(9002, CPendingSubscriptionBatch{ { 7070, 202 }, _TyRequest::MARKET_SUBSCRIBE, pending.m_deadline, { quote }, { quote } });
		response.SetId(9002);
		response.SetType(_TyRequest::MARKET_SUBSCRIBE);
		response.SetErrorInfo(1006, "rejected");
		center.OnHQMarketReply(response);
		Check(!market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote) && 202 == LastResponse(event).GetId(), "subscription failure rolls back client subscription");

		market.m_subscriptions.Subscribe(FmtHQSubID(HQOrigin::Client, 7070), { quote });
		market.m_upstreamSubscriptions[quote.String()] = { quote, 9003, _TyRequest::MARKET_SUBSCRIBE, false, pending.m_deadline };
		market.m_pendingBatchSubscriptions.emplace(9003, CPendingSubscriptionBatch{ { 7070, 203 }, _TyRequest::MARKET_SUBSCRIBE, pending.m_deadline, { quote }, { quote } });
		CRequest accepted;
		accepted.SetId(9003);
		accepted.SetType(_TyRequest::MARKET_SUBSCRIBE);
		center.OnHQMarketReply(accepted);
		Check(market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote) && 203 == LastResponse(event).GetId(), "successful subscription ACK retains subscription and original id");
		market.m_upstreamSubscriptions[quote.String()] = { quote, 9004, _TyRequest::MARKET_SUBSCRIBE, false, std::chrono::steady_clock::time_point::min() };
		market.m_pendingBatchSubscriptions.emplace(9004, CPendingSubscriptionBatch{ { 7070, 204 }, _TyRequest::MARKET_SUBSCRIBE, std::chrono::steady_clock::time_point::min(), { quote }, { quote } });
		market.ExpirePendingSubscriptions();
		Check(!market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote) && LastResponse(event).GetErrorInfo().has_value(), "subscription timeout rolls back subscription");

		market.m_subscriptions.Subscribe(FmtHQSubID(HQOrigin::Client, 7070), { quote });
		strategies.m_bMarketAvailable.store(true);
		// Hold the synthetic runtime scheduled so no worker is needed to observe delivery.
		std::shared_ptr<CStrategyRuntime> runtime = std::make_shared<CStrategyRuntime>();
		runtime->m_bAcceptMarketEvents.store(true);
		runtime->m_bScheduled = true;
		runtime->m_config.m_eventQueueLimit = 16;
		strategies.m_runtimes.emplace(7, runtime);
		strategies.m_marketRoutes[quote.String()].insert(7);
		_TyQuoteData data;
		data.mutable_security()->set_symbol("600519");
		data.mutable_security()->set_exchange(hqmarket::market::v1::SSE);
		CRequest push;
		push.SetId(0);
		push.SetType(_TyRequest::MARKET_QUOTE_PUSH);
		push.SetData(data);
		bool bRouted = center.OnHQMarketReply(push);
		bool bDelivered = LastResponse(event).GetData().has_quote() && 1 == runtime->m_events.size();
		strategies.m_runtimes.clear();
		strategies.m_marketRoutes.clear();
		Check(bRouted && bDelivered, "same market push reaches client and strategy even when client service handles it");

		pending.m_deadline = std::chrono::steady_clock::time_point::min();
		market.m_pendingQueries.emplace(key, pending);
		market.ExpirePendingQueries();
		Check(1008 == LastResponse(event).GetErrorInfo()->first && market.m_pendingQueries.empty(), "market timeout completes and removes pending query");
		pending.m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		market.m_pendingQueries.emplace(key, pending);
		center.OnHQMarketStateChanged(SessionState::Disconnected, "test disconnect");
		Check(!strategies.IsMarketAvailable() && market.m_pendingQueries.empty(), "HQ disconnect invalidates strategy market and completes client queries");
		Check(1007 == LastResponse(event).GetErrorInfo()->first, "HQ disconnect retains unavailable error");
		center.OnHQMarketStateChanged(SessionState::Ready, "");
		Check(strategies.IsMarketAvailable() && market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote), "HQ reconnect retains subscriptions and restores availability");
		market.m_pendingQueries.emplace(key, pending);
		center.HandleClientDisconnected(7070);
		Check(market.m_pendingQueries.empty() && !market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote), "client disconnect clears market requests and subscriptions");
	}

public:
	static void Run(bufferevent* event)
	{
		CSession broker(CLoginInfo{});
		CAccountService accounts(&broker);
		Check(accounts.Initialize(), "account service initializes independently");
		CTradeService trader(&accounts, &broker);
		Check(trader.Initialize(), "trade service initializes without strategy service");
		trader.m_refresh.request_stop();
		trader.m_wake.notify_all();
		trader.m_refresh.join();
		accounts.SetAccountStateHandler(std::bind_front(&CTradeService::OnAccountState, &trader));
		CSession hq(CLoginInfo{});
		CHQService market(&hq);
		CStrategyService strategies(&market, &trader, &accounts);
		net::CTcpServer tcpServer(0);
		CEngineCenter engine(&tcpServer, &hq, &broker, &market, &strategies, &trader, &accounts);
		Check(engine.Initialize(), "center binds client and both Session callbacks before startup");
		CRequest request;
		request.SetId(123);
		request.SetConnectionId(7070);
		request.SetType(_TyRequest::TRADE_ACCOUNT_LOGIN);
		engine.OnRequest(request);
		Check("authentication required" == LastResponse(event).GetErrorInfo()->second, "unauthenticated account requests remain rejected");
		engine.m_clientTokens.emplace("test-token", std::chrono::steady_clock::now() + std::chrono::hours(1));
		engine.m_tokenUsers.emplace("test-token", "tester");
		CRequest authentication;
		authentication.SetConnectionId(7070);
		authentication.SetType(_TyRequest::AUTH_LOGIN);
		authentication.SetExtraData("token", "test-token");
		engine.OnRequest(authentication);
		std::pair<bool, std::string> authenticatedClient = engine.IsAuthenticated(7070);
		Check(authenticatedClient.first && "tester" == authenticatedClient.second, "token authentication restores the client user");
		engine.HandleClientDisconnected(7070);
		Check(!engine.IsAuthenticated(7070).first, "disconnect removes the authenticated client identity");
		engine.OnRequest(authentication);
		Check("tester" == engine.m_authClients.at(7070), "token reconnect binds the user again");
		for (_TyRequest type : { _TyRequest::TRADE_ACCOUNT_LOGIN, _TyRequest::TRADE_ACCOUNT_LIST_QUERY, _TyRequest::TRADE_FUNDS_QUERY, _TyRequest::SUPPORTED_BROKER_QUERY })
		{
			request.SetType(type);
			engine.OnRequest(request);
			CRequest response = LastResponse(event);
			Check(123 == response.GetId() && type == response.GetType(), "direct account route preserves request identity");
			Check("XBroker is unavailable" == response.GetErrorInfo()->second, "account route reaches trade service without a strategy service");
		}
		TestBrokerRouting(event, engine, accounts, trader);
		TestMarketRouting(event, engine, market, strategies);
		engine.OnRequest(authentication);
		_TyAccount account{ 1, "000123" };
		CAccountInfo entry;
		entry = FmtAccountInfo(account);
		entry.m_state = TradeAccountState::Connected;
		trade::CAccountData& cached = trader.m_snapshots[account];
		cached.m_bReady = true;
		cached.m_account.m_account = account;
		cached.m_account.m_bConnected = true;
		cached.m_account.m_bFundsValid = true;
		accounts.m_accounts[account].m_info = std::move(entry);
		Check(accounts.AcquireAccount(account), "ready account permits strategy use");
		Check(accounts.AcquireAccount(account), "multiple consumers can use one account");
		request.SetType(_TyRequest::TRADE_ACCOUNT_DISCONNECT);
		WriteAccountIdentity(account, request);
		engine.OnRequest(request);
		Check("Account is disconnecting or in use" == LastResponse(event).GetErrorInfo()->second, "direct account route preserves strategy disconnect guard");
		accounts.ReleaseAccount(account);
		engine.OnRequest(request);
		Check("Account is disconnecting or in use" == LastResponse(event).GetErrorInfo()->second, "remaining consumer still blocks disconnect");
		accounts.ReleaseAccount(account);
		accounts.ReleaseAccount(account);
		engine.OnRequest(request);
		Check("XBroker is unavailable" == LastResponse(event).GetErrorInfo()->second, "disconnect forwards after strategies stop");
		Check(accounts.AcquireAccount(account), "failed forwarding releases the disconnect guard");
		accounts.ReleaseAccount(account);
		{
			CStrategyService strategies(nullptr, &trader, &accounts);
			Check(strategies.BeginAccountUse(account, 7), "strategy registers account use");
			Check(!strategies.BeginAccountUse(account, 7), "duplicate strategy does not acquire twice");
			strategies.EndAccountUse(account, 7);
			strategies.EndAccountUse(account, 7);
			Check(0 == accounts.m_accounts.at(account).m_useCount, "strategy releases account use exactly once");
			strategies.StopAll();
			Check(accounts.AcquireAccount(account), "stopping strategies does not stop the shared trade service");
			accounts.ReleaseAccount(account);
		}
		Check(accounts.AcquireAccount(account), "catalogue refresh starts with an occupied account");
		accounts.m_accounts.at(account).m_bDisconnecting = true;
		request::TradeAccountList catalogue;
		request::TradeAccountRecord* record = catalogue.add_accounts();
		record->set_broker_id(account.first);
		record->set_account(account.second);
		record->set_state(static_cast<request::TradingAccountState>(TradeAccountState::Connected));
		CRequest catalogueResponse;
		catalogueResponse.SetData(catalogue);
		accounts.OnAccountListReply(catalogueResponse, accounts.m_broker_connect_id);
		Check(1 == accounts.m_accounts.at(account).m_useCount && accounts.m_accounts.at(account).m_bDisconnecting, "catalogue refresh preserves local runtime guards");
		Check(!accounts.AcquireAccount(account), "disconnecting account rejects new use");
		catalogue.clear_accounts();
		catalogueResponse.SetData(catalogue);
		accounts.OnAccountListReply(catalogueResponse, accounts.m_broker_connect_id);
		Check(!accounts.HasAccount(account) && accounts.GetAccounts().empty(), "removed runtime is hidden from the account directory");
		Check(1 == accounts.m_accounts.at(account).m_useCount && accounts.m_accounts.at(account).m_bDisconnecting, "missing account retains outstanding use and disconnect guards");
		accounts.ReleaseAccount(account);
		Check(accounts.m_accounts.contains(account), "disconnect guard retains runtime after final release");
		accounts.FinishDisconnect(account);
		Check(!accounts.m_accounts.contains(account), "runtime is removed after both guards finish");
		accounts.m_accounts[account].m_info = FmtAccountInfo(account);
		accounts.m_accounts.at(account).m_info.m_state = TradeAccountState::Connected;
		trader.Stop();
		Check(accounts.IsConnected(account), "stopping trade service leaves account state available");
		Check(accounts.AcquireAccount(account), "account service remains usable after trade service stops");
		accounts.ReleaseAccount(account);
		accounts.Stop();
		broker.Stop();
		broker.ClearHandlers();
		CTradeService unavailable(nullptr, nullptr);
		Check(!unavailable.Initialize(), "trade service requires an injected account service");
	}
};

int main()
{
	std::unique_ptr<event_base, decltype(&event_base_free)> base(event_base_new(), event_base_free);
	if (nullptr == base)
	{
		return 1;
	}
	bufferevent* event = bufferevent_socket_new(base.get(), -1, 0);
	sockaddr_storage address{};
	address.ss_family = AF_INET;
	if (nullptr == net::CNetPool::InstanceRef().RegisterAConnection(7070, event, &address))
	{
		if (nullptr != event)
		{
			bufferevent_free(event);
		}
		return 1;
	}
	int result = 0;
	try
	{
		CServiceRoutingTests::Run(event);
		std::cout << "Service routing tests passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		result = 1;
	}
	net::CNetPool::InstanceRef().CloseAConnection(7070);
	return result;
}
