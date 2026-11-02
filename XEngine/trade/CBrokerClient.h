#pragma once
#include "TradeTypes.h"
#include <functional>
#include "../system/CSession.h"
#include <atomic>
#include <unordered_map>
class CBrokerClient final
{
public:
	using _TyReplyHandler = std::function<void(const trade::CCounterReply&)>;
	using _TyEventHandler = std::function<void(const trade::CCountEvent&)>;
	using _TyStateHandler = std::function<void(SessionState)>;
	using _TyWireHandler = std::function<void(const CRequest&)>;
	explicit CBrokerClient(const CLoginInfo& info);
	~CBrokerClient();
	bool Start();
	bool Forward(CRequest request, _TyWireHandler handler);
	void SetStateHandler(_TyStateHandler&& handler);
	bool IsReady() const;
	trade::CResult SendRequest(trade::CReqParam request, _TyReplyHandler handler);
	void SetEventHandler(_TyEventHandler handler);
	void Stop();

private:
	friend class CBrokerClientTests;
	void CompleteRequest(const CRequest& response, const _TyAccount& accountIndex, std::uint64_t nRequestId, trade::Operation operation, std::uint64_t nGeneration, const _TyReplyHandler& handler);
	struct CPending
	{
		_TyWireHandler m_handler;
		std::chrono::steady_clock::time_point m_deadline;
		_TyRequest m_type{ _TyRequest::UNKNOWN };
	};
	void OnResponse(const CRequest& response);
	void OnState(SessionState state);
	void Refresh();
	void FailPending();
	CSession m_session;
	mutable std::mutex m_mtx_state;
	std::unordered_map<std::uint64_t, CPending> m_pending;
	std::uint64_t m_generation{ 0 };
	_TyEventHandler m_eventHandler;
	_TyStateHandler m_stateHandler;
	std::atomic_uint64_t m_nNextId{ 1 };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;
};
