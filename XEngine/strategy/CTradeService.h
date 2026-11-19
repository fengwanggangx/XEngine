#pragma once
#include "CAccountService.h"
#include "../trade/TradingTypes.h"
#include "../trade/StrategyAccountView.h"
#include <array>
#include <optional>
#include <unordered_map>

// 同步账户资金、持仓、委托和成交，维护交易快照并处理报撤单及交易回报。
class CTradeService final
{
	friend class CServiceRoutingTests;
	friend class CBrokerServiceTests;

public:
	using _TyAccountReadyHandler = std::function<void(const _TyAccount&)>;
	using _TyEventHandler = std::function<void(const trade::CCounterEvent&)>;
	using _TyStrategyReplyHandler = std::function<void(const CRequest&, const trade::CCounterReply&)>;

	~CTradeService();
	CTradeService(CAccountService* pAccountService, CSession* pSession);

	void Stop();
	bool Initialize();
	const std::string& GetLastError() const;

	bool OnReply(const CRequest& req);
	bool OnRequest(const CRequest& req);
	void OnBrokerEvent(trade::CCounterEvent ev);
	void OnAccountState(const _TyAccount& account);
	trade::CResult SendStrategyRequest(CRequest&& request);
	void OnBrokerState(SessionState state, const std::string& reason = {});

	void SetEventHandler(_TyEventHandler&& handler);
	void SetPushHandler(_TyRequestNotify&& handler);
	void SetAccountReadyHandler(_TyAccountReadyHandler&& handler);
	void SetStrategyReplyHandler(_TyStrategyReplyHandler&& handler);

	bool IsReady(const _TyAccount& account) const;
	bool HasActiveOrders(const _TyAccount& account) const;
	CAccountSnapshot GetAccount(const _TyAccount& account) const;
	trade::CAccountData Snapshot(const _TyAccount& account) const;
	std::vector<trade::COrder> GetOrders(const _TyAccount& account) const;
	CPositionSnapshot GetPosition(const _TyAccount& account, const CSecurity& security) const;
	CStrategyAccountView GetStrategyAccountView(const _TyAccount& account, const CSecurity& security) const;
	std::optional<trade::CExecutionRule> GetExecutionRules(const _TyAccount& account, const CSecurity& security);

	void ReconcileAccount(const _TyAccount& account);

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
	CSession* m_p_session{ nullptr };
	CAccountService* m_p_account_service{ nullptr };
	std::atomic_bool m_b_running{ false };
	std::string m_str_last_error;
	mutable std::mutex m_mtx_state;
	std::map<_TyAccount, trade::CAccountData> m_snapshots;

	struct CSync
	{
		std::vector<trade::CCounterEvent> m_events;
		trade::CAccountData m_data;
		std::array<std::uint64_t, 5> m_versions{};
		std::uint64_t m_cycle{ 0 };
		std::size_t m_completed{ 0 };
		bool m_b_overflow{ false };
	};

	std::unordered_map<_TyAccount, CSync, CAccountHash> m_syncing;

	struct CRuleCache
	{
		std::optional<trade::CExecutionRule> m_value;
		std::chrono::steady_clock::time_point m_retry;
		std::uint64_t m_request_id{ 0 };
	};

	std::map<std::pair<_TyAccount, std::string>, CRuleCache> m_rules;
	std::uint64_t m_generation{ 0 };
	std::unordered_map<_TyAccount, std::uint64_t, CAccountHash> m_account_epochs;
	std::uint64_t m_next_account_epoch{ static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) };
	std::atomic_uint64_t m_n_next_sync_id{ 1 };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;

	std::mutex m_mtx_handlers;
	_TyStrategyReplyHandler m_strategy_reply_handler;
	_TyEventHandler m_event_handler;
	_TyRequestNotify m_push_handler;
	_TyAccountReadyHandler m_account_ready_handler;
	std::unordered_map<_TyAccount, std::uint64_t, CAccountHash> m_last_event_sequence;
};
