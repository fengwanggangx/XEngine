#include "../CTradeService.h"
#include "../CStrategyService.h"
#include "../../system/CEngineService.h"
#include "../../request/request.pb.h"
#include "../../network/CNetPool.h"
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
		CEngineService engine(nullptr, nullptr, nullptr, &trader, &accounts);
		CRequest request;
		request.SetId(123);
		request.SetConnectionId(7070);
		request.SetType(_TyRequest::TRADE_ACCOUNT_LOGIN);
		engine.OnClientRequest(7070, request);
		Check("authentication required" == LastResponse(event).GetErrorInfo()->second, "unauthenticated account requests remain rejected");
		engine.m_client_tokens.emplace("test-token", std::chrono::steady_clock::now() + std::chrono::hours(1));
		engine.m_tokenUsers.emplace("test-token", "tester");
		CRequest authentication;
		authentication.SetConnectionId(7070);
		authentication.SetType(_TyRequest::AUTH_LOGIN);
		authentication.SetExtraData("token", "test-token");
		engine.OnClientRequest(7070, authentication);
		std::pair<bool, std::string> authenticatedClient = engine.IsAuthenticated(7070);
		Check(authenticatedClient.first && "tester" == authenticatedClient.second, "token authentication restores the client user");
		engine.HandleDisconnected(7070);
		Check(!engine.IsAuthenticated(7070).first, "disconnect removes the authenticated client identity");
		engine.OnClientRequest(7070, authentication);
		Check("tester" == engine.m_auth_clients.at(7070), "token reconnect binds the user again");
		for (_TyRequest type : { _TyRequest::TRADE_ACCOUNT_LOGIN, _TyRequest::TRADE_ACCOUNT_LIST_QUERY, _TyRequest::TRADE_FUNDS_QUERY, _TyRequest::SUPPORTED_BROKER_QUERY })
		{
			request.SetType(type);
			engine.OnClientRequest(7070, request);
			CRequest response = LastResponse(event);
			Check(123 == response.GetId() && type == response.GetType(), "direct account route preserves request identity");
			Check("XBroker is unavailable" == response.GetErrorInfo()->second, "account route reaches trade service without a strategy service");
		}
		_TyAccount account{ 1, "000123" };
		CAccountInfo entry;
		entry = AccountInfo(account);
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
		engine.OnClientRequest(7070, request);
		Check("Account is disconnecting or in use" == LastResponse(event).GetErrorInfo()->second, "direct account route preserves strategy disconnect guard");
		accounts.ReleaseAccount(account);
		engine.OnClientRequest(7070, request);
		Check("Account is disconnecting or in use" == LastResponse(event).GetErrorInfo()->second, "remaining consumer still blocks disconnect");
		accounts.ReleaseAccount(account);
		accounts.ReleaseAccount(account);
		engine.OnClientRequest(7070, request);
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
		accounts.OnCatalogue(catalogueResponse, accounts.m_generation);
		Check(1 == accounts.m_accounts.at(account).m_useCount && accounts.m_accounts.at(account).m_bDisconnecting, "catalogue refresh preserves local runtime guards");
		Check(!accounts.AcquireAccount(account), "disconnecting account rejects new use");
		catalogue.clear_accounts();
		catalogueResponse.SetData(catalogue);
		accounts.OnCatalogue(catalogueResponse, accounts.m_generation);
		Check(!accounts.HasAccount(account) && accounts.GetAccounts().empty(), "removed runtime is hidden from the account directory");
		Check(1 == accounts.m_accounts.at(account).m_useCount && accounts.m_accounts.at(account).m_bDisconnecting, "missing account retains outstanding use and disconnect guards");
		accounts.ReleaseAccount(account);
		Check(accounts.m_accounts.contains(account), "disconnect guard retains runtime after final release");
		accounts.FinishDisconnect(account);
		Check(!accounts.m_accounts.contains(account), "runtime is removed after both guards finish");
		accounts.m_accounts[account].m_info = AccountInfo(account);
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
