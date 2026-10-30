#pragma once
#include "ITradeSession.h"
#include "../system/CSession.h"
#include <atomic>
#include <array>
#include <unordered_map>
#include <map>
class CBrokerClient final : public std::enable_shared_from_this<CBrokerClient>
{
public:
	using _TyReplyHandler = trade::ITradeSession::_TyReplyHandler;
	using _TyEventHandler = trade::ITradeSession::_TyEventHandler;
	std::shared_ptr<trade::ITradeSession> CreateSession(const _TyAccount& account);
	using _TyWireHandler = std::function<void(const CRequest&)>;
	explicit CBrokerClient(const CLoginInfo& info);
	~CBrokerClient();
	bool Start();
	bool Forward(CRequest request, _TyWireHandler handler);
	void SetCatalogueHandler(_TyWireHandler handler);
	trade::CResult SendRequest(trade::CReqParam request, _TyReplyHandler handler);
	void SetEventHandler(_TyEventHandler handler);
	trade::CAccountData Snapshot(const _TyAccount& accountIndex) const;
	void Stop();

private:
	std::map<_TyAccount, std::weak_ptr<class CBrokerSession>> m_sessions;
	friend class CBrokerClientTests;
	void PublishSession(const trade::CCountEvent& event);
	void CompleteRequest(const CRequest& response, const _TyAccount& accountIndex, std::uint64_t nRequestId, trade::Operation operation, std::uint64_t nGeneration, const _TyReplyHandler& handler);
	struct CPending
	{
		_TyWireHandler m_handler;
		std::chrono::steady_clock::time_point m_deadline;
		_TyRequest m_type{ _TyRequest::UNKNOWN };
	};
	void OnResponse(const CRequest& response);
	void ApplyEvent(trade::CAccountData& data, trade::CCountEvent& event);
	void OnState(SessionState state);
	void Refresh();
	void QueryAccount(const _TyAccount& accountIndex);
	void QueryNext(const _TyAccount& accountIndex, std::uint64_t nGeneration, std::uint64_t nCycle, trade::Operation operation);
	void CompleteQuery(const trade::CCounterReply& reply, std::uint64_t nGeneration, std::uint64_t nCycle);
	void FailPending();
	CSession m_session;
	mutable std::mutex m_mtx_state;
	std::unordered_map<std::uint64_t, CPending> m_pending;
	std::map<_TyAccount, trade::CAccountData> m_snapshots;
	struct CSync
	{
		std::vector<trade::CCountEvent> m_events;
		trade::CAccountData m_data;
		std::array<std::uint64_t, 5> m_versions{};
		std::uint64_t m_cycle{ 0 };
		std::size_t m_completed{ 0 };
		bool m_bOverflow{ false };
	};
	std::map<_TyAccount, CSync> m_syncing;
	std::uint64_t m_generation{ 0 };
	_TyEventHandler m_eventHandler;
	_TyWireHandler m_catalogueHandler;
	std::atomic_uint64_t m_nNextId{ 1 };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;
};
