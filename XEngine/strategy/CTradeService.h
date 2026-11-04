#pragma once
#include "CAccountService.h"
#include "../trade/TradingTypes.h"
#include <array>
#include <optional>
#include <unordered_map>

class CTradeService final
{
	friend class CServiceRoutingTests;
	friend class CBrokerServiceTests;

public:
	bool OnReply(const CRequest& req);
	using _TyStrategyReplyHandler = std::function<void(const CRequest&, const trade::CCounterReply&)>;
	using _TyEventHandler = std::function<void(const trade::CCounterEvent&)>;
	using _TyAccountReadyHandler = std::function<void(const _TyAccount&)>;

	void Stop();
	~CTradeService();
	bool Initialize();
	bool IsReady(const _TyAccount& account) const;
	void OnAccountState(const _TyAccount& account);
	void OnBrokerEvent(trade::CCounterEvent event);
	void SetEventHandler(_TyEventHandler&& handler);
	void SetPushHandler(_TyRequestNotify&& handler);
	bool HasActiveOrders(const _TyAccount& account) const;
	CAccountSnapshot GetAccount(const _TyAccount& account) const;
	trade::CAccountData Snapshot(const _TyAccount& account) const;
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);
	CTradeService(CAccountService* pAccountService, CSession* pSession);
	std::vector<trade::COrder> GetOrders(const _TyAccount& account) const;
	void OnBrokerState(SessionState state, const std::string& reason = {});
	CPositionSnapshot GetPosition(const _TyAccount& account, const CSecurity& security) const;
	const std::string& GetLastError() const;
	bool OnRequest(const CRequest& req);
	trade::CResult SendStrategyRequest(CRequest&& request);
	void SetStrategyReplyHandler(_TyStrategyReplyHandler&& handler);

private:
	void RefreshTrades();
	void QueryAccount(const _TyAccount& account);
	void PublishEvent(const trade::CCounterEvent& event);
	void ApplyEvent(trade::CAccountData& data, trade::CCounterEvent& event);
	void CompleteQuery(const trade::CCounterReply& reply, std::uint64_t nGeneration, std::uint64_t nCycle);
	void QueryNext(const _TyAccount& account, std::uint64_t nGeneration, std::uint64_t nCycle, trade::Operation operation);

	void FailPending();
	void RefreshPending();

	struct CPending
	{
		CRequest m_origin;
		std::chrono::steady_clock::time_point m_deadline;
	};

	bool SendRequest(CRequest&& request);
	bool DispatchReply(const CRequest& origin, const CRequest& reply);

	std::mutex m_mtx_requests;
	std::unordered_map<_TyRequestId, CPending> m_pending;
	CSession* m_pSession{ nullptr };
	CAccountService* m_pAccountService{ nullptr };
	std::atomic_bool m_bRunning{ false };
	std::string m_strLastError;
	mutable std::mutex m_mtx_state;
	std::map<_TyAccount, trade::CAccountData> m_snapshots;
	struct CSync
	{
		std::vector<trade::CCounterEvent> m_events;
		trade::CAccountData m_data;
		std::array<std::uint64_t, 5> m_versions{};
		std::uint64_t m_cycle{ 0 };
		std::size_t m_completed{ 0 };
		bool m_bOverflow{ false };
	};
	std::map<_TyAccount, CSync> m_syncing;
	std::uint64_t m_generation{ 0 };
	std::atomic_uint64_t m_nNextSyncId{ 1 };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;

	std::mutex m_mtx_handlers;
	_TyStrategyReplyHandler m_strategyReplyHandler;
	_TyEventHandler m_eventHandler;
	_TyRequestNotify m_pushHandler;
	_TyAccountReadyHandler m_accountReadyHandler;
	std::map<_TyAccount, std::uint64_t> m_lastEventSequence;
};
