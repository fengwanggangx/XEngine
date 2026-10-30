#include "../CTradeService.h"
#include "../CStrategyService.h"
#include "../../system/CEngineService.h"
#include "../../network/CNetPool.h"
#include "../../network/CFrameBuffer.h"
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <iostream>
#include <stdexcept>

class CServiceRoutingTests
{
	class CSession final : public trade::ITradeSession
	{
	public:
		explicit CSession(const _TyAccount& account) : m_account(account)
		{
		}

		trade::CResult SendRequest(trade::CReqParam, _TyReplyHandler) override
		{
			return {};
		}

		void SetEventHandler(_TyEventHandler) override
		{
		}

		void Close() override
		{
		}

		trade::CAccountData Snapshot() const override
		{
			trade::CAccountData data;
			data.m_bReady = true;
			data.m_account.m_accountIndex = m_account;
			data.m_account.m_bConnected = true;
			data.m_account.m_bFundsValid = true;
			return data;
		}

	private:
		_TyAccount m_account;
	};

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
		CTradeService trader;
		Check(trader.Initialize(), "trade service initializes without strategy service");
		CEngineService engine(nullptr, nullptr, nullptr, &trader);
		CRequest request;
		request.SetId(123);
		request.SetConnectionId(7070);
		request.SetType(_TyRequest::TRADE_ACCOUNT_LOGIN);
		engine.OnClientRequest(7070, request);
		Check("authentication required" == LastResponse(event).GetErrorInfo()->second, "unauthenticated account requests remain rejected");
		engine.m_auth_clients.emplace(7070);
		engine.m_clientUsers.emplace(7070, "tester");
		for (_TyRequest type : { _TyRequest::TRADE_ACCOUNT_LOGIN, _TyRequest::TRADE_ACCOUNT_LIST_QUERY, _TyRequest::TRADE_FUNDS_QUERY, _TyRequest::SUPPORTED_BROKER_QUERY })
		{
			request.SetType(type);
			engine.OnClientRequest(7070, request);
			CRequest response = LastResponse(event);
			Check(123 == response.GetId() && type == response.GetType(), "direct account route preserves request identity");
			Check("XBroker is unavailable" == response.GetErrorInfo()->second, "account route reaches trade service without a strategy service");
		}
		_TyAccount account{ 1, "000123" };
		CTradeAccount entry;
		entry.m_info = AccountInfo(account);
		entry.m_info.m_state = TradeAccountState::Connected;
		// The map and active operations share the account session lifetime.
		entry.m_session = std::make_shared<CSession>(account);
		trader.m_accounts.emplace(account, std::move(entry));
		Check(trader.BeginStrategy(account, 7), "ready account permits strategy use");
		request.SetType(_TyRequest::TRADE_ACCOUNT_DISCONNECT);
		WriteAccountIdentity(account, request);
		engine.OnClientRequest(7070, request);
		Check("Account is disconnecting or has running strategies" == LastResponse(event).GetErrorInfo()->second, "direct account route preserves strategy disconnect guard");
		trader.EndStrategy(account, 7);
		engine.OnClientRequest(7070, request);
		Check("XBroker is unavailable" == LastResponse(event).GetErrorInfo()->second, "disconnect forwards after strategies stop");
		Check(trader.BeginStrategy(account, 8), "failed forwarding releases the disconnect guard");
		trader.EndStrategy(account, 8);
		{
			CStrategyService strategies(nullptr, &trader);
			strategies.StopAll();
			Check(trader.BeginStrategy(account, 9), "stopping strategies does not stop the shared trade service");
			trader.EndStrategy(account, 9);
		}
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
