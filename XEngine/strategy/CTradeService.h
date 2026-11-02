#pragma once
#include "CAccountService.h"
#include "../trade/TradingTypes.h"
#include <array>

class CTradeService final
{
	friend class CServiceRoutingTests;
	friend class CBrokerClientTests;

public:
	using _TyEventHandler = std::function<void(const trade::CCountEvent&)>;
	using _TyPushHandler = std::function<void(const CRequest&)>;
	using _TyAccountReadyHandler = std::function<void(const _TyAccount&)>;
	CTradeService(CAccountService* pAccountService, CBrokerClient* pBroker);
	~CTradeService();
	bool Initialize();
	void Stop();
	bool IsReady(const _TyAccount& accountIndex) const;
	trade::CAccountData Snapshot(const _TyAccount& accountIndex) const;
	CAccountSnapshot GetAccount(const _TyAccount& accountIndex) const;
	CPositionSnapshot GetPosition(const _TyAccount& accountIndex, const CSecurity& security) const;
	std::vector<trade::COrder> GetOrders(const _TyAccount& accountIndex) const;
	bool HasActiveOrders(const _TyAccount& accountIndex) const;
	void SetEventHandler(_TyEventHandler&& handler);
	void SetPushHandler(_TyPushHandler&& handler);
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);
	void OnAccountState(const _TyAccount& accountIndex);
	void OnBrokerEvent(trade::CCountEvent event);
	void OnBrokerState(SessionState state);
	const std::string& GetLastError() const;
	bool HandleTradingRequest(const CRequest& req);
	trade::CResult SendRequest(trade::CReqParam request, CBrokerClient::_TyReplyHandler handler);

private:
	void RefreshTrades();
	void PublishEvent(const trade::CCountEvent& event);
	void ApplyEvent(trade::CAccountData& data, trade::CCountEvent& event);
	void QueryAccount(const _TyAccount& accountIndex);
	void QueryNext(const _TyAccount& accountIndex, std::uint64_t nGeneration, std::uint64_t nCycle, trade::Operation operation);
	void CompleteQuery(const trade::CCounterReply& reply, std::uint64_t nGeneration, std::uint64_t nCycle);
	CAccountService* m_pAccountService{ nullptr };
	CBrokerClient* m_pBroker{ nullptr };
	std::atomic_bool m_bRunning{ false };
	std::string m_strLastError;
	mutable std::mutex m_mtx_state;
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
	std::atomic_uint64_t m_nNextSyncId{ 1 };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;

	std::mutex m_mtx_handlers;
	_TyEventHandler m_eventHandler;
	_TyPushHandler m_pushHandler;
	_TyAccountReadyHandler m_accountReadyHandler;
	std::map<_TyAccount, std::uint64_t> m_lastEventSequence;
};
