#pragma once

#include <array>

#include "../trade/StrategyAccountView.h"

#include "../database/CDBEngine.h"
#include <memory>
#include <map>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <tuple>

class CExecutionDatabase;

enum class ExecutionState
{
	Prepared = 0,
	Queued = 1,
	Unknown = 2,
	BrokerActive = 3,
	TerminalAwaitingSync = 4,
	Closed = 5,
	NotSent = 6
};

// 保存交易意图、执行阶段、资源预留及柜台订单核对结果。
struct CExecutionRecord
{
	COrderIntent m_intent;
	ExecutionState m_state{ ExecutionState::Prepared };
	std::int64_t m_reserved_money{ 0 };
	std::uint64_t m_order_id{ 0 };
	std::uint64_t m_evidence_version{ 0 };
	bool m_broker_covered{ false };
	std::string m_message;
	std::int64_t m_filled_quantity{ 0 };
	trade::Status m_broker_status{ trade::Status::Unknown };
};

// One MySQL session owns execution admission for the database.
// A durable transaction must commit before the caller may send an order.
// 管理交易执行准入、资源预留、执行核对、检查点和退出保护，持久化后才允许发单。
class CStrategyExecutionManager final
{
public:
	CStrategyExecutionManager();
	~CStrategyExecutionManager();
	CStrategyExecutionManager(const CStrategyExecutionManager& arg) = delete;
	CStrategyExecutionManager& operator=(const CStrategyExecutionManager& arg) = delete;

	bool Open()
	{
		return Open(CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql));
	}

	bool Open(db::_TyDBPtr pDB);
	std::string GetLastError() const;
	bool HasUnresolved(_TyStrategyId strategyId) const;
	// Pending in-memory executions only; terminal history remains in MySQL.
	std::vector<CExecutionRecord> GetRecords() const;
	bool Reconcile(const _TyAccount& account, const CStrategyAccountView& view);
	bool MarkSubmission(std::uint64_t clientId, bool queued, bool definitelyNotSent = false);
	COrderSubmitResult Prepare(COrderIntent intent, const CStrategyAccountView& view, std::int64_t maximumDebit);

	bool IsBuyLocked(const _TyAccount& account, const CSecurity& security) const;
	bool LockBuys(const _TyAccount& account, const CSecurity& security, _TyStrategyId ownerId, const std::string& reason);
	bool UnlockBuys(const _TyAccount& account, const CSecurity& security, _TyStrategyId ownerId);
	std::uint64_t AllocateSignal();
	std::optional<CExecutionRecord> FindSignal(_TyStrategyId id, std::uint64_t signalId) const;
	bool RejectBeforeAcceptance(std::uint64_t clientId, const std::string& reason);
	std::optional<CExecutionRecord> FindExecution(std::uint64_t clientId) const;
	std::string LoadCheckpoint(_TyStrategyId id, const std::string& key = {}) const;
	bool SaveCheckpoint(_TyStrategyId id, const _TyAccount& account, const CSecurity& security, const std::string& data, bool protect, bool scoped = false);
	bool ClaimExit(const _TyAccount& account, const CSecurity& security, _TyStrategyId id);
	bool ReleaseExit(const _TyAccount& account, const CSecurity& security, _TyStrategyId id);
	std::int64_t AvailableFunds(const _TyAccount& account, const CStrategyAccountView& view) const;

private:
	using _TyLockKey = std::tuple<_TyAccount, std::string, _TyStrategyId>;
	void Load();
	void Save(const CExecutionRecord& record);
	bool ReconcileWithoutLock(const _TyAccount& account, const CStrategyAccountView& view);
	bool IsBuyLockedWithoutLock(const _TyAccount& account, const CSecurity& security) const;
	static bool IsUnresolved(ExecutionState state);

	mutable std::mutex m_state_mutex;
	std::unique_ptr<CExecutionDatabase> m_database;
	bool m_healthy{ false };
	std::string m_last_error;
	std::map<std::uint64_t, CExecutionRecord> m_records;
	std::map<_TyLockKey, std::string> m_locks;
	std::unordered_map<_TyAccount, std::array<std::uint64_t, 4>, CAccountHash> m_snapshotVersions;
	std::unordered_map<_TyAccount, std::uint64_t, CAccountHash> m_snapshot_epochs;
	std::map<std::pair<_TyAccount, std::string>, _TyStrategyId> m_exit_owners;
};
