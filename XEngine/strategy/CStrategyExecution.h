#pragma once

#include "../trade/StrategyAccountView.h"

#include "../database/CODBC.h"
#include <memory>
#include <map>
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

struct CExecutionRecord
{
	COrderIntent m_intent;
	ExecutionState m_state{ ExecutionState::Prepared };
	std::int64_t m_reservedMoney{ 0 };
	std::uint64_t m_orderId{ 0 };
	std::uint64_t m_evidenceVersion{ 0 };
	bool m_bBrokerCovered{ false };
	std::string m_message;
	std::int64_t m_filledQuantity{ 0 };
	trade::Status m_brokerStatus{ trade::Status::Unknown };
};

// One MySQL session owns execution admission for the database.
// A durable transaction must commit before the caller may send an order.
class CStrategyExecution final
{
public:
	CStrategyExecution();
	~CStrategyExecution();
	CStrategyExecution(const CStrategyExecution& arg) = delete;
	CStrategyExecution& operator=(const CStrategyExecution& arg) = delete;

	bool Open(db::_TyDBPtr connection);
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

	mutable std::mutex m_mtx_state;
	std::unique_ptr<CExecutionDatabase> m_database;
	bool m_bHealthy{ false };
	std::string m_strLastError;
	std::map<std::uint64_t, CExecutionRecord> m_records;
	std::map<_TyLockKey, std::string> m_locks;
	std::map<_TyAccount, std::uint64_t> m_snapshotVersions;
	std::map<_TyAccount, std::uint64_t> m_snapshotEpochs;
	std::map<std::pair<_TyAccount, std::string>, _TyStrategyId> m_exitOwners;
};
