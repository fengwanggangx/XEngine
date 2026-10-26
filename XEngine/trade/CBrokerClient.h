#pragma once
#include "ITradeCounter.h"
#include "../system/CSession.h"
#include <atomic>
#include <unordered_map>
class CBrokerClient final : public trade::ITradeCounter
{
	public:
	using WireHandler = std::function<void(const CRequest&)>;
	explicit CBrokerClient(const CLoginInfo& info);
	~CBrokerClient() override;
	bool Start();
	bool Forward(CRequest request, WireHandler handler);
	void SetCatalogueHandler(WireHandler handler);
	trade::CResult Request(trade::CRequest request, ReplyHandler handler) override;
	void SetEventHandler(EventHandler handler) override;
	trade::CSnapshot Snapshot(trade::Id nAccountId = 0, bool bHistory = true) const override;
	std::vector<trade::Operation> Capabilities() const override;
	void Stop() override;

	private:
	struct CPending
	{
		WireHandler m_handler;
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
	std::unordered_map<trade::Id, trade::CSnapshot> m_snapshots;
	EventHandler m_eventHandler;
	WireHandler m_catalogueHandler;
	std::atomic_uint64_t m_nNextId{ 1 };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;
};
