#ifndef XENGINE_STRATEGY_CSTRATEGYENGINE_H
#define XENGINE_STRATEGY_CSTRATEGYENGINE_H

#include "IStrategy.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <map>
#include <unordered_set>
#include <variant>
#include <vector>

class CRequest;
class CSession;
class CStrategyContext;
class CTradeService;
enum class SessionState;

enum class StrategyEventType
{
	Start,
	Pause,
	Stop,
	Quote,
	Depth,
	MarketTrade,
	Bar,
	Order,
	Trade,
	Timer
};

using _TyStrategyEventData = std::variant<std::monostate, _TyQuoteData, _TyDepthData, _TyMarketTradeData, _TyBarData, COrderEvent, CTradeEvent, CTimerEvent>;

struct CStrategyEvent
{
	CStrategyEvent();
	StrategyEventType m_type{ StrategyEventType::Quote };
	std::uint64_t m_sequence{ 0 };
	_TyStrategyEventData m_data;
	std::shared_ptr<std::promise<bool>> m_completion;

	bool IsMarketEvent() const;
};

struct CStrategyRuntime
{
	CStrategyConfig m_config;
	std::unique_ptr<IStrategy> m_strategy;
	std::unique_ptr<CStrategyContext> m_context;
	std::atomic<StrategyState> m_state{ StrategyState::Created };
	std::atomic_bool m_bAcceptMarketEvents{ false };
	std::atomic_bool m_bRemoving{ false };

	std::mutex m_mtx_events;
	std::condition_variable m_cvIdle;
	std::deque<CStrategyEvent> m_events;
	bool m_bScheduled{ false };
	bool m_bExecutingCallback{ false };

	std::mutex m_mtx_orders;
	std::unordered_set<_TyOrderId> m_activeOrderIds;

	std::mutex m_mtx_metrics;
	std::uint64_t m_lastEventSequence{ 0 };
	std::uint64_t m_droppedMarketEvents{ 0 };
	std::int64_t m_lastCallbackDurationUs{ 0 };
	std::int64_t m_maxCallbackDurationUs{ 0 };
	std::string m_strLastError;
};

struct CSubscriptionEntry
{
	CQuoteInfo m_quote;
	std::unordered_set<_TyStrategyId> m_strategyIds;
	bool m_bSubscribed{ false };
	bool m_bSubscribing{ false };
};

struct CSignalKey
{
	_TyStrategyId m_strategyId{ 0 };
	_TySignalId m_signalId{ 0 };

	bool operator==(const CSignalKey& arg) const;
};

struct CSignalKeyHash
{
	std::size_t operator()(const CSignalKey& key) const;
};

enum class SignalState
{
	Pending,
	Accepted,
	Rejected
};

class CStrategyEngine final
{
	friend class CStrategyContext;
	using _TyTradingPushHandler = std::function<void(const CRequest&)>;
	using _TyStateHandler = std::function<void(const CStrategySnapshot&)>;

public:
	explicit CStrategyEngine(CSession* pSession);
	~CStrategyEngine();
	CStrategyEngine(const CStrategyEngine& arg) = delete;
	CStrategyEngine& operator=(const CStrategyEngine& arg) = delete;

public:
	bool Initialize();
	bool HandleStrategyRequest(const CRequest& req);
	bool HandleTradingRequest(const CRequest& req);
	void StopTrading();
	void SetTradingPushHandler(_TyTradingPushHandler&& handler);
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	std::unique_ptr<IStrategy> CreateStrategy(const std::string& strType) const;
	bool CreateStrategy(const CStrategyConfig& config);
	bool RemoveStrategy(_TyStrategyId id);

	const std::string& GetLastError() const;

public:
	bool StartStrategy(_TyStrategyId id);
	bool PauseStrategy(_TyStrategyId id);
	bool StopStrategy(_TyStrategyId id);
	void StopAll();

public:
	std::optional<CStrategySnapshot> GetSnapshot(_TyStrategyId id) const;
	std::vector<CStrategySnapshot> GetSnapshots() const;
	void RegisterStateHandler(_TyStateHandler&& handler);
	bool IsMarketAvailable() const;

private:
	void OnHQMarketResponse(const CRequest& req);
	void OnHQMarketState(SessionState state, const std::string& strReason);
	void OnOrderEvent(const COrderEvent& ev);
	void OnTradeEvent(const CTradeEvent& ev);

private:
	std::shared_ptr<CStrategyRuntime> FindRuntime(_TyStrategyId id) const;
	bool ExecuteControl(_TyStrategyId id, StrategyEventType type);
	bool EnqueueEvent(const std::shared_ptr<CStrategyRuntime>& runtime, CStrategyEvent&& ev);
	void DrainEvents(const std::shared_ptr<CStrategyRuntime>& runtime);
	bool DispatchEvent(CStrategyRuntime& runtime, CStrategyEvent& ev);
	void HandleStrategyException(CStrategyRuntime& runtime, const std::string& strError);
	void RouteMarketEvent(const std::string& strKey, CStrategyEvent&& ev);
	void AddRoutes(const CStrategyConfig& cfg);
	void RemoveRoutes(const CStrategyConfig& cfg);
	void SubscribeRequiredQuotes(const CStrategyConfig& cfg);
	void UnsubscribeUnusedQuotes(const CStrategyConfig& cfg);
	void RestoreRequiredSubscriptions();
	COrderSubmitResult SubmitOrder(_TyStrategyId id, const COrderIntent& intent);
	bool CancelOrder(_TyStrategyId id, _TyOrderId orderId);
	CStrategySnapshot MakeSnapshot(const std::shared_ptr<CStrategyRuntime>& runtime) const;
	void NotifySnapshot(const std::shared_ptr<CStrategyRuntime>& runtime);
	void WaitUntilIdle(const std::shared_ptr<CStrategyRuntime>& runtime);
	static std::string GetMarketKey(const CRequest& req);
	static bool IsFinishedOrderStatus(OrderStatus status);
	bool LoadStrategies();
	bool AddStrategy(const CRequest& req);
	bool ModifyStrategy(const CRequest& req);
	bool QueryStrategies(const CRequest& req) const;
	bool DeleteStrategy(const CRequest& req);
	bool QueryRuntime(const CRequest& req) const;
	bool ControlStrategy(const CRequest& req);

private:
	std::mutex m_mtx_operations;
	CSession* m_pSession{ nullptr };
	std::unique_ptr<CTradeService> m_trader;

	mutable std::shared_mutex m_mtx_strategies;
	std::unordered_map<_TyStrategyId, std::shared_ptr<CStrategyRuntime>> m_runtimes;

	mutable std::shared_mutex m_mtx_routes;
	std::unordered_map<std::string, std::unordered_set<_TyStrategyId>> m_marketRoutes;
	std::unordered_map<std::string, CSubscriptionEntry> m_subscriptions;

	mutable std::shared_mutex m_mtx_orders;
	using _TyOrderRouteIndex = std::pair<_TyAccount, _TyOrderId>;
	std::map<_TyOrderRouteIndex, _TyStrategyId> m_orderRoutes;
	std::map<_TyOrderRouteIndex, _TyStrategyId> m_clientOrderRoutes;

	std::mutex m_mtx_signals;
	std::unordered_map<CSignalKey, SignalState, CSignalKeyHash> m_signals;

	std::mutex m_mtx_handlers;
	std::vector<_TyStateHandler> m_stateHandlers;

	std::atomic_bool m_bMarketAvailable{ false };
	std::atomic_bool m_bStopping{ false };
	std::string m_strLastError;
};

#endif
