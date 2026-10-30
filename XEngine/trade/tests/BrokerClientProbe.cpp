#include "../CBrokerClient.h"
#include <future>
#include <iostream>
static _TyAccount TestAccount(std::uint64_t number)
{
	return 0 == number ? _TyAccount{} : _TyAccount{ 1, std::to_string(number) };
}

int main(int nArgs, char** pArgs)
{
	bool bFault = 5 == nArgs;
	if (4 != nArgs && !bFault)
	{
		return 1;
	}
	net::EnvInitialize();
	CLoginInfo info;
	info.m_strToken = pArgs[2];
	info.m_host.m_strName = "Broker";
	info.m_host.m_strHost = "127.0.0.1";
	info.m_host.m_nPort = static_cast<unsigned int>(std::stoul(pArgs[1]));
	_TyAccount accountIndex = { 1, pArgs[3] };
	CBrokerClient client(info);
	std::atomic_int nEvents{ 0 };
	std::atomic_bool bTradeCorrelated{ false };
	client.SetEventHandler([&nEvents, &bTradeCorrelated](const trade::CCountEvent& event)
	{
		if (trade::EventTy::Deal == event.m_kind && 700 == event.m_order.m_clientId && 77 == event.m_order.m_strategyId)
		{
			bTradeCorrelated.store(true);
		}
		if (trade::EventTy::Order == event.m_kind)
		{
			++nEvents;
		}
	});
	if (!client.Start())
	{
		return 2;
	}
	for (int nIndex = 0; nIndex < 100 && !client.Snapshot(accountIndex).m_bReady; ++nIndex)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (!client.Snapshot(accountIndex).m_bReady)
	{
		return 3;
	}
	trade::CReqParam request;
	request.m_accountIndex = accountIndex;
	request.m_param = trade::CSubmitParam{};
	request.m_requestId = 700;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_clientId = 700;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_accountIndex = accountIndex;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_strategyId = 77;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_signalId = 78;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_strSecurity = "600000.SSE";
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_nPrice = 1000;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_nPriceScale = 2;
	std::get<trade::CSubmitParam>(request.m_param).m_order.m_nQuantity = 100;
	std::promise<trade::CCounterReply> completion;
	std::future<trade::CCounterReply> future = completion.get_future();
	if (!client.SendRequest(std::move(request), [&completion](const trade::CCounterReply& reply)
	{
		completion.set_value(reply);
	}).Ok())
	{
		return 4;
	}
	if (std::future_status::ready != future.wait_for(std::chrono::seconds(5)))
	{
		return 5;
	}
	trade::CCounterReply reply = future.get();
	if (bFault)
	{
		client.Stop();
		if (trade::Error::UnknownResult != reply.m_result.m_error || client.Snapshot(accountIndex).m_bReady)
		{
			return 9;
		}
		std::cout << "XEngine interrupted order reports UnknownResult and invalidates cache\n";
		return 0;
	}
	if (!reply.m_result.Ok() || 700 != reply.m_requestId || !std::holds_alternative<trade::COperationReceipt>(reply.m_data) || 0 == std::get<trade::COperationReceipt>(reply.m_data).m_orderId || 0 == nEvents.load())
	{
		return 6;
	}
	// A submit response is partial: it must not replace the synchronized position cache.
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	if (client.Snapshot(accountIndex).m_positions.empty() || !bTradeCorrelated.load())
	{
		return 7;
	}
	client.Stop();
	if (client.Snapshot(accountIndex).m_bReady)
	{
		return 8;
	}
	std::cout << "XEngine XBroker client authentication, synchronization, async order correlation, events and shutdown passed\n";
}
