#pragma once

#include "../trade/StrategyAccountView.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <tuple>

struct sqlite3;

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

struct CExecutionRecord
{
	COrderIntent m_intent;
	ExecutionState m_state{ ExecutionState::Prepared };
	std::int64_t m_reservedMoney{ 0 };
	std::uint64_t m_orderId{ 0 };
	std::uint64_t m_evidenceVersion{ 0 };
	bool m_bBrokerCovered{ false };
	std::string m_message;
};

// One journal has one owning engine. No network calls are made under its lock.
// SQLite commit must succeed before the caller is allowed to send an order.
class CStrategyExecution final
{
public:
	CStrategyExecution() = default;
	~CStrategyExecution();
	CStrategyExecution(const CStrategyExecution& arg) = delete;
	CStrategyExecution& operator=(const CStrategyExecution& arg) = delete;

	bool Open(const std::filesystem::path& path);
	std::string GetLastError() const;
	bool HasUnresolved(_TyStrategyId strategyId) const;
	// Pending in-memory executions only; terminal history remains in SQLite.
	std::vector<CExecutionRecord> GetRecords() const;
	bool Reconcile(const _TyAccount& account, const CStrategyAccountView& view);
	bool MarkSubmission(std::uint64_t clientId, bool queued, bool definitelyNotSent = false);
	COrderSubmitResult Prepare(COrderIntent intent, const CStrategyAccountView& view, std::int64_t maximumDebit);

	bool IsBuyLocked(const _TyAccount& account, const CSecurity& security) const;
	bool LockBuys(const _TyAccount& account, const CSecurity& security, _TyStrategyId ownerId, const std::string& reason);
	bool UnlockBuys(const _TyAccount& account, const CSecurity& security, _TyStrategyId ownerId);

private:
	using _TyLockKey = std::tuple<_TyAccount, std::string, _TyStrategyId>;
	void Load();
	void Save(const CExecutionRecord& record);
	bool ReconcileWithoutLock(const _TyAccount& account, const CStrategyAccountView& view);
	bool IsBuyLockedWithoutLock(const _TyAccount& account, const CSecurity& security) const;
	static bool IsUnresolved(ExecutionState state);

	mutable std::mutex m_mtx_state;
	sqlite3* m_database{ nullptr };
	bool m_bHealthy{ false };
	std::string m_strLastError;
	std::map<std::uint64_t, CExecutionRecord> m_records;
	std::map<_TyLockKey, std::string> m_locks;
	std::map<_TyAccount, std::uint64_t> m_snapshotVersions;
};
