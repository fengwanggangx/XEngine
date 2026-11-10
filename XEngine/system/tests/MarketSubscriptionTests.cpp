// Offline transport for exercising the real CHQService state machine.
// This executable never links or starts the production Session implementation.
#include "../CHQService.h"
#include "../CSession.h"
#include "../../network/CNetTools.h"
#include "../../request/RequestCenter.h"
#include "../../request/request.pb.h"
#include "../../request/v1/market.pb.h"
#include <iostream>
#include <stdexcept>

namespace
{
	std::vector<CRequest> sent;
	std::vector<CRequest> responses;
	std::vector<net::_TyConnectionId> responseIds;
	bool sendAccepted{ true };
	bool authenticated{ true };

	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	CRequest Reply(const CRequest& request, bool accepted = true)
	{
		CRequest reply;
		reply.SetId(request.GetId());
		reply.SetType(request.GetType());
		_TySubscriptionAck ack;
		CRequest::_TyExtraRows rows;
		if (request.GetExtraData("subscriptions").empty())
		{
			rows.emplace_back(std::unordered_map<std::string, std::string>{ { "security", request.GetExtraData("security") }, { "channel", request.GetExtraData("channel") } });
		}
		else
		{
			Check(request.GetExtraData("subscriptions", rows), "upstream batch parses");
		}
		for (const auto& row : rows)
		{
			hqmarket::market::v1::SubscriptionResult* result = ack.add_results();
			result->mutable_security()->set_symbol(row.at("security").substr(0, row.at("security").find('.')));
			result->mutable_security()->set_exchange(hqmarket::market::v1::SSE);
			result->set_channel(static_cast<hqmarket::market::v1::Channel>(ParseChannel(row.at("channel"))));
			result->set_accepted(accepted);
			result->set_reason(accepted ? "" : "test rejection");
		}
		reply.SetData(ack);
		return reply;
	}

	CRequest ClientRequest(const CQuoteInfo& quote, bool subscribe, _TyRequestId id)
	{
		CRequest request = subscribe ? request::Subscription(quote) : request::UnSubscription(quote);
		request.SetConnectionId(7070);
		request.SetId(id);
		return request;
	}
} // namespace

// The forward-declared transport is never instantiated by this test Session.
namespace net
{
	class CTcpClient
	{
	};

	bool SendRequest(_TyConnectionId id, const CRequest& request)
	{
		responseIds.emplace_back(id);
		responses.emplace_back(request);
		return true;
	}

	void SetError(CRequest& request, int code, const std::string& message)
	{
		request.SetErrorInfo(code, message);
	}

	void SendError(_TyConnectionId id, const CRequest& request, int code, const std::string& message)
	{
		CRequest response = request;
		response.SetErrorInfo(code, message);
		SendRequest(id, response);
	}
} // namespace net

CSession::CSession(const CLoginInfo& info) : m_login(info)
{
}

CSession::~CSession() = default;

bool CSession::IsAuthenticated() const
{
	return authenticated;
}

bool CSession::SendRequest(const CRequest& request)
{
	sent.emplace_back(request);
	return sendAccepted;
}

class CMarketSubscriptionTests
{
public:
	static void Run()
	{
		CQuoteInfo quote("600519", Exchange::sse, Channel::quote);
		{
			CSubscriptionMgr subscriptions;
			std::string client = FmtHQSubID(HQOrigin::Client, 7070);
			std::string strategy = FmtHQSubID(HQOrigin::Strategy, 7070);
			Check("client_7070" == client && "strategy_7070" == strategy, "subscriber keys preserve type and numeric id");
			HQOrigin type = HQOrigin::Unknown;
			std::uint64_t id = 0;
			Check(ParseHQSubID(client, type, id) && HQOrigin::Client == type && 7070 == id, "client subscriber key parses");
			Check(ParseHQSubID("strategy_18446744073709551615", type, id) && HQOrigin::Strategy == type && UINT64_MAX == id, "subscriber keys preserve the full uint64 id range");
			Check(!ParseHQSubID("client_18446744073709551616", type, id) && !ParseHQSubID("client_7070extra", type, id) && !ParseHQSubID("client_07070", type, id), "malformed or noncanonical subscriber keys are rejected");
			Check(1 == subscriptions.Subscribe(client, { quote }).size(), "first typed subscriber requires upstream subscription");
			Check(subscriptions.Subscribe(strategy, { quote }).empty(), "same numeric strategy id shares upstream without aliasing client");
			subscriptions.Subscribe(strategy, { quote });
			Check(2 == subscriptions.GetSubscriptionCount(quote), "typed subscribers deduplicate by both type and id");
			Check(subscriptions.RemoveSubscriber(client).empty() && subscriptions.IsSubscribed(strategy, quote), "client release preserves same-id strategy");
			Check(1 == subscriptions.RemoveSubscriber(strategy).size(), "last typed subscriber releases upstream");
		}
		CSession session(CLoginInfo{});
		{
			CHQService market(&session);
			market.Subscribe(HQOrigin::Strategy, 7070, { quote });
			market.OnRequest(ClientRequest(quote, true, 90));
			market.OnReply(Reply(sent.back()));
			responses.clear();
			responseIds.clear();
			_TyQuoteData data;
			data.mutable_security()->set_symbol("600519");
			data.mutable_security()->set_exchange(hqmarket::market::v1::SSE);
			CRequest push;
			push.SetId(0);
			push.SetType(_TyRequest::MARKET_QUOTE_PUSH);
			push.SetData(data);
			market.OnReply(push);
			Check(1 == responses.size() && 7070 == responseIds.front(), "only client subscribers are sent network pushes");
			market.HandleClientDisconnected(7070);
			Check(1 == sent.size() && market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Strategy, 7070), quote), "client disconnect cannot release a same-id strategy");
			market.Release(HQOrigin::Strategy, 7070);
			Check(2 == sent.size() && _TyRequest::MARKET_UNSUBSCRIBE == sent.back().GetType(), "shared upstream releases only after both typed users leave");
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			market.Subscribe(HQOrigin::Strategy, 1, { quote });
			market.Subscribe(HQOrigin::Strategy, 2, { quote });
			Check(1 == sent.size(), "strategies share one upstream subscribe");
			market.OnRequest(ClientRequest(quote, true, 101));
			Check(responses.empty(), "terminal joining an in-flight strategy subscription waits for ACK");
			Check(!market.m_upstreamSubscriptions.at(quote.String()).m_bConfirmed, "write success does not confirm subscription");
			market.OnReply(Reply(sent.back()));
			Check(1 == responses.size() && !responses.back().GetErrorInfo().has_value(), "shared ACK completes terminal subscription");
			market.Release(HQOrigin::Strategy, 1);
			market.OnRequest(ClientRequest(quote, false, 102));
			Check(1 == sent.size(), "terminal unsubscribe and first strategy stop retain the other strategy");
			market.Release(HQOrigin::Strategy, 2);
			Check(2 == sent.size() && _TyRequest::MARKET_UNSUBSCRIBE == sent.back().GetType(), "last consumer sends one unsubscribe");
			CRequest unsubscribe = sent.back();
			market.Subscribe(HQOrigin::Strategy, 3, { quote });
			Check(2 == sent.size(), "new demand waits for an in-flight unsubscribe");
			market.OnReply(Reply(unsubscribe));
			Check(3 == sent.size() && _TyRequest::MARKET_SUBSCRIBE == sent.back().GetType(), "unsubscribe ACK resubscribes new demand");
			Check(!market.OnReply(Reply(unsubscribe)), "duplicate ACK cannot complete a newer operation");
			market.OnReply(Reply(sent.back()));
			market.OnRequest(ClientRequest(quote, true, 103));
			Check(4 == sent.size(), "terminal joining confirmed demand requests a fresh HQMarket snapshot");
			market.OnReply(Reply(sent.back()));
			market.HandleClientDisconnected(7070);
			Check(4 == sent.size(), "terminal disconnect retains the strategy subscription");
			market.OnSessionState(SessionState::Disconnected, "offline");
			Check(!market.m_upstreamSubscriptions.at(quote.String()).m_bConfirmed, "disconnect invalidates confirmation");
			market.OnSessionState(SessionState::Ready, "");
			Check(5 == sent.size(), "reconnect restores shared strategy demand exactly once");
			market.OnReply(Reply(sent.back()));
			market.Release(HQOrigin::Strategy, 3);
			market.OnReply(Reply(sent.back()));
			Check(market.m_upstreamSubscriptions.empty(), "last unsubscribe ACK releases upstream state");
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			market.OnRequest(ClientRequest(quote, true, 150));
			market.OnReply(Reply(sent.back()));
			market.Subscribe(HQOrigin::Strategy, 1, { quote });
			Check(1 == sent.size(), "strategy reuses an already confirmed terminal subscription");
			CQuoteInfo depth("600519", Exchange::sse, Channel::depth);
			market.Subscribe(HQOrigin::Strategy, 2, { depth });
			Check(2 == sent.size(), "different channels have independent upstream subscriptions");
			market.OnReply(Reply(sent.back()));
			market.Release(HQOrigin::Strategy, 2);
			Check(3 == sent.size() && "depth" == sent.back().GetExtraData("channel"), "depth release does not affect quote demand");
			market.OnReply(Reply(sent.back()));
			market.OnRequest(ClientRequest(quote, false, 151));
			Check(3 == sent.size(), "terminal release retains strategy quote demand");
			market.Release(HQOrigin::Strategy, 1);
			Check(4 == sent.size(), "last quote consumer releases the remaining channel");
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			market.OnRequest(ClientRequest(quote, true, 201));
			market.Subscribe(HQOrigin::Strategy, 1, { quote });
			market.OnReply(Reply(sent.back(), false));
			Check(responses.back().GetErrorInfo().has_value(), "per-item rejection fails terminal even without top-level error");
			Check(!market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote), "rejection rolls back terminal demand");
			Check(market.HasDemand(quote) && !market.m_upstreamSubscriptions.at(quote.String()).m_bConfirmed, "strategy demand remains unconfirmed for reconnect");
			market.OnSessionState(SessionState::Disconnected, "offline");
			market.OnSessionState(SessionState::Ready, "");
			Check(2 == sent.size(), "reconnect retries retained strategy demand");
			market.OnReply(Reply(sent.back()));
			market.Release(HQOrigin::Strategy, 1);
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			market.OnRequest(ClientRequest(quote, true, 301));
			CRequest stale = sent.back();
			market.OnRequest(ClientRequest(quote, false, 302));
			market.OnRequest(ClientRequest(quote, true, 303));
			market.OnReply(Reply(stale));
			Check(market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote), "superseded request cannot remove fresh terminal demand");
			Check(303 == responses.back().GetId() && !responses.back().GetErrorInfo().has_value(), "latest terminal request receives the ACK");
			market.HandleClientDisconnected(7070);
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			market.Subscribe(HQOrigin::Strategy, 1, { quote });
			market.OnRequest(ClientRequest(quote, true, 401));
			market.m_upstreamSubscriptions.at(quote.String()).m_deadline = std::chrono::steady_clock::time_point::min();
			market.ExpirePendingSubscriptions();
			Check(responses.back().GetErrorInfo().has_value(), "timeout completes terminal waiter");
			Check(market.HasDemand(quote), "terminal timeout preserves strategy demand");
			market.Release(HQOrigin::Strategy, 1);
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			market.OnSessionState(SessionState::Disconnected, "offline");
			market.Subscribe(HQOrigin::Strategy, 1, { quote });
			Check(sent.empty(), "offline strategy demand does not write to the session");
			market.Release(HQOrigin::Strategy, 1);
			market.OnSessionState(SessionState::Ready, "");
			Check(sent.empty(), "strategy stopped offline is not restored on reconnect");
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			CRequest batch;
			batch.SetConnectionId(7070);
			batch.SetId(450);
			batch.SetType(_TyRequest::MARKET_SUBSCRIBE);
			CRequest::_TyExtraRows rows{
				{ { "security", "600519.SSE" }, { "channel", "quote" } },
				{ { "security", "600000.SSE" }, { "channel", "quote" } }
			};
			batch.SetExtraData("subscriptions", rows);
			market.OnRequest(batch);
			Check(1 == sent.size() && responses.empty(), "client batch uses one upstream request and waits for ACK");
			CRequest reply = Reply(sent.front());
			_TySubscriptionAck ack = reply.GetData().subscription_ack();
			ack.mutable_results(0)->set_accepted(false);
			ack.mutable_results(0)->set_reason("test rejection");
			reply.SetData(ack);
			market.OnReply(reply);
			Check(responses.back().GetErrorInfo().has_value(), "one rejected batch item fails the request");
			Check(2 == sent.size() && _TyRequest::MARKET_UNSUBSCRIBE == sent.back().GetType(), "accepted item in a rolled-back batch is released upstream");
			market.OnReply(Reply(sent.back()));
			Check(market.m_upstreamSubscriptions.empty(), "batch rollback leaves no orphan subscriptions");
		}

		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			CRequest batch;
			batch.SetConnectionId(7070);
			batch.SetId(460);
			batch.SetType(_TyRequest::MARKET_SUBSCRIBE);
			CRequest::_TyExtraRows rows{
				{ { "security", "600519.SSE" }, { "channel", "quote" } },
				{ { "security", "600000.SSE" }, { "channel", "quote" } },
				{ { "security", "600519.SSE" }, { "channel", "quote" } }
			};
			batch.SetExtraData("subscriptions", rows);
			market.OnRequest(batch);
			Check(1 == sent.size(), "duplicate securities share one upstream batch");
			CRequest reply = Reply(sent.back());
			Check(2 == reply.GetData().subscription_ack().results_size(), "upstream batch deduplicates securities");
			market.OnReply(reply);
			Check(1 == responses.size() && !responses.back().GetErrorInfo().has_value(), "all batch results complete the client ACK");
			batch.SetId(461);
			batch.SetConnectionId(8080);
			market.OnRequest(batch);
			Check(2 == sent.size(), "new client refreshes cached snapshots with one upstream batch");
			reply = Reply(sent.back());
			_TySubscriptionAck ack = reply.GetData().subscription_ack();
			ack.mutable_results()->RemoveLast();
			reply.SetData(ack);
			market.OnReply(reply);
			Check(responses.back().GetErrorInfo().has_value(), "missing batch result fails the new request");
			Check(market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote), "failed snapshot refresh preserves existing subscriber");
		}
		sent.clear();
		responses.clear();
		{
			CHQService market(&session);
			sendAccepted = false;
			market.OnRequest(ClientRequest(quote, true, 501));
			Check(responses.back().GetErrorInfo().has_value() && !market.m_subscriptions.IsSubscribed(FmtHQSubID(HQOrigin::Client, 7070), quote), "write failure completes and rolls back terminal request");
			sendAccepted = true;
		}
	}
};

int main()
{
	try
	{
		CMarketSubscriptionTests::Run();
		std::cout << "Market subscription tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
