#include "CStrategyExecution.h"

#if defined(_WIN32)
#include <winsqlite/winsqlite3.h>
#else
#include <sqlite3.h>
#endif

#include <algorithm>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>

namespace
{
	using _TyStatement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

	void Execute(sqlite3* database, const char* sql)
	{
		if (SQLITE_OK != sqlite3_exec(database, sql, nullptr, nullptr, nullptr))
		{
			throw std::runtime_error(sqlite3_errmsg(database));
		}
	}

	_TyStatement Statement(sqlite3* database, const char* sql)
	{
		sqlite3_stmt* statement = nullptr;
		int result = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
		_TyStatement owner(statement, sqlite3_finalize);
		if (SQLITE_OK != result)
		{
			throw std::runtime_error(sqlite3_errmsg(database));
		}
		return owner;
	}

	void Bind(sqlite3_stmt* statement, int index, std::int64_t value)
	{
		if (SQLITE_OK != sqlite3_bind_int64(statement, index, value))
		{
			throw std::runtime_error("Cannot bind journal integer");
		}
	}

	void Bind(sqlite3_stmt* statement, int index, const std::string& value)
	{
		if ((value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) || (SQLITE_OK != sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT)))
		{
			throw std::runtime_error("Cannot bind journal text");
		}
	}

	std::string ReadText(sqlite3_stmt* statement, int index)
	{
		const unsigned char* text = sqlite3_column_text(statement, index);
		return nullptr == text ? std::string() : std::string(reinterpret_cast<const char*>(text), sqlite3_column_bytes(statement, index));
	}

	void Done(sqlite3* database, sqlite3_stmt* statement)
	{
		if (SQLITE_DONE != sqlite3_step(statement))
		{
			throw std::runtime_error(sqlite3_errmsg(database));
		}
	}

	bool Active(trade::Status status)
	{
		return (trade::Status::Accepted == status) || (trade::Status::PartiallyFilled == status) || (trade::Status::Pending == status) || (trade::Status::Unknown == status);
	}

	bool ValidIntent(const COrderIntent& intent)
	{
		std::uint64_t maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
		bool directionValid = ((OrderSide::Buy == intent.m_side) && (PositionEffect::Open == intent.m_positionEffect)) || ((OrderSide::Sell == intent.m_side) && ((PositionEffect::Close == intent.m_positionEffect) || (PositionEffect::CloseToday == intent.m_positionEffect)));
		return intent.IsValid() && IsAccountValid(intent.m_account) && directionValid && (OrderType::Limit == intent.m_orderType) && (0 <= intent.m_nPriceScale) && (8 >= intent.m_nPriceScale) && (maximum >= intent.m_id) && (maximum >= intent.m_signalId) && (maximum >= intent.m_clientOrderId);
	}

	bool SameOrder(const COrderIntent& intent, const trade::COrder& order)
	{
		return (intent.m_account == order.m_account) && (intent.m_id == order.m_id) && (intent.m_signalId == order.m_signalId) && (intent.m_security.String() == order.m_strSecurity) && ((OrderSide::Buy == intent.m_side ? trade::Side::Buy : trade::Side::Sell) == order.m_side) && (intent.m_price == order.m_nPrice) && (intent.m_nPriceScale == order.m_nPriceScale) && (intent.m_quantity == order.m_nQuantity);
	}
} // namespace

CStrategyExecution::~CStrategyExecution()
{
	if (nullptr != m_database)
	{
		sqlite3_close(m_database);
	}
}

bool CStrategyExecution::Open(const std::filesystem::path& path)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (nullptr != m_database)
	{
		m_strLastError = "Journal is already open";
		return false;
	}
	try
	{
		std::u8string pathBytes = path.u8string();
		if (path.empty() || (SQLITE_OK != sqlite3_open_v2(reinterpret_cast<const char*>(pathBytes.c_str()), &m_database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr)))
		{
			throw std::runtime_error("Cannot open strategy execution journal");
		}
		sqlite3_busy_timeout(m_database, 100);
		// Retain the exclusive connection lock across commits. A second engine must fail.
		Execute(m_database, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA locking_mode=EXCLUSIVE;");
		_TyStatement version = Statement(m_database, "PRAGMA user_version");
		if ((SQLITE_ROW != sqlite3_step(version.get())) || (0 > sqlite3_column_int(version.get(), 0)) || ((0 != sqlite3_column_int(version.get(), 0)) && (2 != sqlite3_column_int(version.get(), 0))))
		{
			throw std::runtime_error("Unsupported strategy journal version");
		}
		version.reset();
		Execute(m_database, "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS execution (client_id INTEGER PRIMARY KEY AUTOINCREMENT, account TEXT NOT NULL, strategy_id INTEGER NOT NULL, signal_id INTEGER NOT NULL, security TEXT NOT NULL, exchange_id INTEGER NOT NULL, side INTEGER NOT NULL, price INTEGER NOT NULL, price_scale INTEGER NOT NULL, quantity INTEGER NOT NULL, effect INTEGER NOT NULL, state INTEGER NOT NULL, reserved_money INTEGER NOT NULL, order_id INTEGER NOT NULL, evidence INTEGER NOT NULL, covered INTEGER NOT NULL, message TEXT NOT NULL, filled INTEGER NOT NULL DEFAULT 0, broker_status INTEGER NOT NULL DEFAULT 7, UNIQUE(account,strategy_id,signal_id)); CREATE TABLE IF NOT EXISTS protection (account TEXT NOT NULL, security TEXT NOT NULL, owner_id INTEGER NOT NULL, reason TEXT NOT NULL, PRIMARY KEY(account,security,owner_id)); CREATE TABLE IF NOT EXISTS signal_sequence(id INTEGER PRIMARY KEY AUTOINCREMENT); CREATE TABLE IF NOT EXISTS checkpoint(strategy_id INTEGER PRIMARY KEY, data TEXT NOT NULL); CREATE TABLE IF NOT EXISTS exit_owner(account TEXT NOT NULL, security TEXT NOT NULL, owner_id INTEGER NOT NULL, PRIMARY KEY(account,security)); PRAGMA user_version=2; COMMIT;");
		// Reserve a durable, monotonically increasing client ID range.
		std::int64_t floor = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		_TyStatement sequence = Statement(m_database, "INSERT INTO sqlite_sequence(name,seq) SELECT 'execution',? WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name='execution')");
		Bind(sequence.get(), 1, floor);
		Done(m_database, sequence.get());
		sequence = Statement(m_database, "UPDATE sqlite_sequence SET seq=MAX(seq,?) WHERE name='execution'");
		Bind(sequence.get(), 1, floor);
		Done(m_database, sequence.get());
		Load();
		m_bHealthy = true;
		m_strLastError.clear();
		return true;
	}
	catch (const std::exception& error)
	{
		m_strLastError = error.what();
		if (nullptr != m_database)
		{
			sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
			sqlite3_close(m_database);
			m_database = nullptr;
		}
		m_records.clear();
		m_locks.clear();
		return false;
	}
}

void CStrategyExecution::Load()
{
	_TyStatement statement = Statement(m_database, "SELECT client_id,account,strategy_id,signal_id,security,exchange_id,side,price,price_scale,quantity,effect,state,reserved_money,order_id,evidence,covered,message FROM execution WHERE state NOT IN (5,6)");
	int result = SQLITE_ROW;
	while (SQLITE_ROW == (result = sqlite3_step(statement.get())))
	{
		CExecutionRecord record;
		COrderIntent& intent = record.m_intent;
		intent.m_clientOrderId = sqlite3_column_int64(statement.get(), 0);
		if (!DecodeAccount(ReadText(statement.get(), 1), intent.m_account))
		{
			throw std::runtime_error("Invalid account in strategy journal");
		}
		intent.m_id = sqlite3_column_int64(statement.get(), 2);
		intent.m_signalId = sqlite3_column_int64(statement.get(), 3);
		intent.m_security = CSecurity(ReadText(statement.get(), 4), static_cast<Exchange>(sqlite3_column_int(statement.get(), 5)));
		intent.m_side = static_cast<OrderSide>(sqlite3_column_int(statement.get(), 6));
		intent.m_price = sqlite3_column_int64(statement.get(), 7);
		intent.m_nPriceScale = sqlite3_column_int(statement.get(), 8);
		intent.m_quantity = sqlite3_column_int64(statement.get(), 9);
		intent.m_positionEffect = static_cast<PositionEffect>(sqlite3_column_int(statement.get(), 10));
		int state = sqlite3_column_int(statement.get(), 11);
		if ((0 > state) || (4 < state) || !ValidIntent(intent) || (0 == intent.m_clientOrderId))
		{
			throw std::runtime_error("Invalid execution in strategy journal");
		}
		// A crash can occur at any point after preparation. Never assume not sent.
		record.m_state = ExecutionState::Unknown;
		record.m_reservedMoney = sqlite3_column_int64(statement.get(), 12);
		record.m_orderId = sqlite3_column_int64(statement.get(), 13);
		record.m_evidenceVersion = 0;
		record.m_bBrokerCovered = false;
		record.m_message = "Restarted; reconcile existing order before retry";
		if (0 > record.m_reservedMoney)
		{
			throw std::runtime_error("Invalid reservation in strategy journal");
		}
		m_records.emplace(intent.m_clientOrderId, std::move(record));
	}
	if (SQLITE_DONE != result)
	{
		throw std::runtime_error(sqlite3_errmsg(m_database));
	}
	statement = Statement(m_database, "SELECT account,security,owner_id,reason FROM protection");
	while (SQLITE_ROW == (result = sqlite3_step(statement.get())))
	{
		_TyAccount account;
		std::int64_t owner = sqlite3_column_int64(statement.get(), 2);
		std::string security = ReadText(statement.get(), 1);
		std::size_t separator = security.rfind('.');
		CSecurity parsed;
		if (std::string::npos != separator)
		{
			parsed = CSecurity(security.substr(0, separator), ParseMarket(security.substr(separator + 1)));
		}
		if (!DecodeAccount(ReadText(statement.get(), 0), account) || (0 >= owner) || !parsed.IsValid() || (security != parsed.String()))
		{
			throw std::runtime_error("Invalid protection lock in strategy journal");
		}
		m_locks.emplace(_TyLockKey{ account, security, static_cast<_TyStrategyId>(owner) }, ReadText(statement.get(), 3));
	}
	if (SQLITE_DONE != result)
	{
		throw std::runtime_error(sqlite3_errmsg(m_database));
	}
	statement = Statement(m_database, "SELECT account,security,owner_id FROM exit_owner");
	while (SQLITE_ROW == (result = sqlite3_step(statement.get())))
	{
		_TyAccount account;
		if (!DecodeAccount(ReadText(statement.get(), 0), account) || (0 >= sqlite3_column_int64(statement.get(), 2)))
		{
			throw std::runtime_error("Invalid exit owner");
		}
		_TyStrategyId ownerId = static_cast<_TyStrategyId>(sqlite3_column_int64(statement.get(), 2));
		std::string security = ReadText(statement.get(), 1);
		if (m_locks.end() == m_locks.find(_TyLockKey{ account, security, ownerId }))
		{
			throw std::runtime_error("Exit owner has no protection lock");
		}
		m_exitOwners.emplace(std::make_pair(account, security), ownerId);
	}
	if (SQLITE_DONE != result)
	{
		throw std::runtime_error(sqlite3_errmsg(m_database));
	}
}

void CStrategyExecution::Save(const CExecutionRecord& record)
{
	_TyStatement statement = Statement(m_database, "UPDATE execution SET state=?,order_id=?,evidence=?,covered=?,message=?,filled=?,broker_status=? WHERE client_id=?");
	Bind(statement.get(), 1, static_cast<std::int64_t>(record.m_state));
	Bind(statement.get(), 2, static_cast<std::int64_t>(record.m_orderId));
	Bind(statement.get(), 3, static_cast<std::int64_t>(record.m_evidenceVersion));
	Bind(statement.get(), 4, static_cast<std::int64_t>(record.m_bBrokerCovered));
	Bind(statement.get(), 5, record.m_message);
	Bind(statement.get(), 6, record.m_filledQuantity);
	Bind(statement.get(), 7, static_cast<std::int64_t>(record.m_brokerStatus));
	Bind(statement.get(), 8, static_cast<std::int64_t>(record.m_intent.m_clientOrderId));
	Done(m_database, statement.get());
	if (1 != sqlite3_changes(m_database))
	{
		throw std::runtime_error("Execution journal row disappeared");
	}
}

bool CStrategyExecution::IsUnresolved(ExecutionState state)
{
	return (ExecutionState::Closed != state) && (ExecutionState::NotSent != state);
}

COrderSubmitResult CStrategyExecution::Prepare(COrderIntent intent, const CStrategyAccountView& view, std::int64_t maximumDebit)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		return { false, false, 0, 0, "Execution journal is unavailable: " + m_strLastError };
	}
	if (!ValidIntent(intent) || (0 != intent.m_clientOrderId))
	{
		return { false, false, 0, 0, "Invalid execution intent" };
	}
	if (!view.m_account.m_bReady || (view.m_identity != intent.m_account) || (view.m_position.m_security != intent.m_security) || (view.m_account.m_version != view.m_position.m_version) || ((view.m_connectionEpoch == m_snapshotEpochs[intent.m_account]) && (view.m_account.m_version < m_snapshotVersions[intent.m_account])))
	{
		return { false, true, 0, 0, "A consistent account snapshot is required" };
	}
	if (!ReconcileWithoutLock(intent.m_account, view))
	{
		return { false, false, 0, 0, "Cannot reconcile execution journal: " + m_strLastError };
	}
	if ((OrderSide::Buy == intent.m_side) && IsBuyLockedWithoutLock(intent.m_account, intent.m_security))
	{
		return { false, false, 0, 0, "Protection task blocks new buys" };
	}
	const auto ownerIter = m_exitOwners.find({ intent.m_account, intent.m_security.String() });
	if ((m_exitOwners.end() != ownerIter) && (ownerIter->second != intent.m_id))
	{
		return { false, true, 0, 0, "Another protection task owns this security" };
	}
	std::int64_t reserved = 0;
	for (const auto& [id, record] : m_records)
	{
		if (!IsUnresolved(record.m_state) || (record.m_intent.m_account != intent.m_account))
		{
			continue;
		}
		if (record.m_intent.m_security == intent.m_security)
		{
			return { false, true, 0, 0, "Security has an unresolved execution" };
		}
		if ((OrderSide::Buy == record.m_intent.m_side) && !record.m_bBrokerCovered)
		{
			if (record.m_reservedMoney > (std::numeric_limits<std::int64_t>::max)() - reserved)
			{
				return { false, false, 0, 0, "Reservation overflow" };
			}
			reserved += record.m_reservedMoney;
		}
	}
	for (const auto& order : view.m_orders)
	{
		if ((order.m_account == intent.m_account) && (order.m_strSecurity == intent.m_security.String()) && Active(order.m_status))
		{
			return { false, true, 0, 0, "Security has an active broker order" };
		}
	}
	if (OrderSide::Buy == intent.m_side)
	{
		// maximumDebit is the caller's validated principal plus fee upper bound.
		// Round principal upward to cents without using floating point.
		std::int64_t scale = 1;
		for (int index = 0; intent.m_nPriceScale > index; ++index)
		{
			scale *= 10;
		}
		if ((intent.m_price > (std::numeric_limits<std::int64_t>::max)() / intent.m_quantity) || (intent.m_price * intent.m_quantity > (std::numeric_limits<std::int64_t>::max)() / 100))
		{
			return { false, false, 0, 0, "Order amount overflow" };
		}
		std::int64_t numerator = intent.m_price * intent.m_quantity * 100;
		std::int64_t principal = numerator / scale + (0 != numerator % scale ? 1 : 0);
		if ((maximumDebit < principal) || (reserved > view.m_account.m_available) || (maximumDebit > view.m_account.m_available - reserved))
		{
			return { false, true, 0, 0, "Insufficient unreserved funds or invalid debit budget" };
		}
	}
	else if (!view.m_position.m_bReady || !view.m_position.m_bFrozenKnown || (intent.m_quantity > view.m_position.m_availableQuantity) || (0 != maximumDebit))
	{
		return { false, true, 0, 0, "Insufficient available position or invalid sell budget" };
	}
	try
	{
		Execute(m_database, "BEGIN IMMEDIATE");
		_TyStatement statement = Statement(m_database, "INSERT INTO execution(account,strategy_id,signal_id,security,exchange_id,side,price,price_scale,quantity,effect,state,reserved_money,order_id,evidence,covered,message) VALUES(?,?,?,?,?,?,?,?,?,?,0,?,0,0,0,'Prepared before send')");
		Bind(statement.get(), 1, EncodeAccount(intent.m_account));
		Bind(statement.get(), 2, static_cast<std::int64_t>(intent.m_id));
		Bind(statement.get(), 3, static_cast<std::int64_t>(intent.m_signalId));
		Bind(statement.get(), 4, intent.m_security.m_strCode);
		Bind(statement.get(), 5, static_cast<std::int64_t>(intent.m_security.m_market));
		Bind(statement.get(), 6, static_cast<std::int64_t>(intent.m_side));
		Bind(statement.get(), 7, intent.m_price);
		Bind(statement.get(), 8, static_cast<std::int64_t>(intent.m_nPriceScale));
		Bind(statement.get(), 9, intent.m_quantity);
		Bind(statement.get(), 10, static_cast<std::int64_t>(intent.m_positionEffect));
		Bind(statement.get(), 11, maximumDebit);
		int result = sqlite3_step(statement.get());
		if ((SQLITE_CONSTRAINT == result) && (SQLITE_CONSTRAINT_UNIQUE == sqlite3_extended_errcode(m_database)))
		{
			Execute(m_database, "ROLLBACK");
			return { false, false, 0, 0, "Duplicate execution signal; reconcile the original order" };
		}
		if (SQLITE_DONE != result)
		{
			throw std::runtime_error(sqlite3_errmsg(m_database));
		}
		intent.m_clientOrderId = sqlite3_last_insert_rowid(m_database);
		CExecutionRecord record;
		record.m_intent = intent;
		record.m_reservedMoney = maximumDebit;
		record.m_message = "Prepared before send";
		// Allocate before commit so allocation failure cannot lose a durable hold.
		m_records.emplace(intent.m_clientOrderId, std::move(record));
		Execute(m_database, "COMMIT");
		return { true, false, intent.m_clientOrderId, 0, "Execution persisted; ready to send" };
	}
	catch (const std::exception& error)
	{
		sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
		m_bHealthy = false;
		m_strLastError = error.what();
		return { false, false, 0, 0, "Execution journal failed: " + m_strLastError };
	}
}

bool CStrategyExecution::MarkSubmission(std::uint64_t clientId, bool queued, bool definitelyNotSent)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	const auto mIter = m_records.find(clientId);
	if (!m_bHealthy || (queued && definitelyNotSent))
	{
		return false;
	}
	if (m_records.end() == mIter)
	{
		try
		{
			_TyStatement statement = Statement(m_database, "SELECT state FROM execution WHERE client_id=?");
			Bind(statement.get(), 1, static_cast<std::int64_t>(clientId));
			int result = sqlite3_step(statement.get());
			if ((SQLITE_ROW != result) && (SQLITE_DONE != result))
			{
				throw std::runtime_error(sqlite3_errmsg(m_database));
			}
			return (SQLITE_ROW == result) && ((static_cast<int>(ExecutionState::Closed) == sqlite3_column_int(statement.get(), 0)) || (static_cast<int>(ExecutionState::NotSent) == sqlite3_column_int(statement.get(), 0)));
		}
		catch (const std::exception& error)
		{
			m_bHealthy = false;
			m_strLastError = error.what();
			return false;
		}
	}
	// A synchronous broker report may already have advanced this execution.
	if ((ExecutionState::Prepared != mIter->second.m_state) && !((ExecutionState::Queued == mIter->second.m_state) && !queued && !definitelyNotSent))
	{
		return true;
	}
	CExecutionRecord changed = mIter->second;
	changed.m_state = queued ? ExecutionState::Queued : (definitelyNotSent ? ExecutionState::NotSent : ExecutionState::Unknown);
	changed.m_message = queued ? "Queued; await broker evidence" : (definitelyNotSent ? "Transport rejected before send" : "Submission result is unknown; do not resend");
	try
	{
		Save(changed);
		mIter->second = std::move(changed);
		if (!IsUnresolved(mIter->second.m_state))
		{
			m_records.erase(mIter);
		}
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

bool CStrategyExecution::Reconcile(const _TyAccount& account, const CStrategyAccountView& view)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	return ReconcileWithoutLock(account, view);
}

bool CStrategyExecution::ReconcileWithoutLock(const _TyAccount& account, const CStrategyAccountView& view)
{
	if (!m_bHealthy || !view.m_account.m_bReady || (account != view.m_identity) || (view.m_connectionEpoch < m_snapshotEpochs[account]) || ((view.m_connectionEpoch == m_snapshotEpochs[account]) && (view.m_account.m_version < m_snapshotVersions[account])))
	{
		return false;
	}
	m_snapshotVersions[account] = view.m_account.m_version;
	try
	{
		if (view.m_connectionEpoch != m_snapshotEpochs[account])
		{
			for (auto& [id, record] : m_records)
			{
				if (account == record.m_intent.m_account)
				{
					CExecutionRecord changed = record;
					changed.m_state = ExecutionState::Unknown;
					changed.m_bBrokerCovered = false;
					changed.m_evidenceVersion = 0;
					Save(changed);
					record = std::move(changed);
				}
			}
			m_snapshotEpochs[account] = view.m_connectionEpoch;
			m_snapshotVersions[account] = view.m_account.m_version;
		}
		std::map<std::uint64_t, const trade::COrder*> orders;
		for (const auto& order : view.m_orders)
		{
			if ((account == order.m_account) && (0 != order.m_clientId) && !orders.emplace(order.m_clientId, &order).second)
			{
				throw std::runtime_error("Duplicate broker client ID in account snapshot");
			}
		}
		for (auto& [id, record] : m_records)
		{
			if (!IsUnresolved(record.m_state) || (account != record.m_intent.m_account))
			{
				continue;
			}
			const auto mIter = orders.find(id);
			// Absence is never proof that a write did not reach the broker.
			if (orders.end() == mIter)
			{
				if (record.m_bBrokerCovered)
				{
					CExecutionRecord changed = record;
					changed.m_state = ExecutionState::Unknown;
					changed.m_bBrokerCovered = false;
					changed.m_message = "Previously observed broker order is missing; reservation retained";
					Save(changed);
					record = std::move(changed);
				}
				continue;
			}
			if (view.m_ordersVersion < record.m_evidenceVersion)
			{
				continue;
			}
			const trade::COrder* pOrder = mIter->second;
			if (!SameOrder(record.m_intent, *pOrder) || (0 == pOrder->m_orderId) || (static_cast<int>(trade::Status::Accepted) > static_cast<int>(pOrder->m_status)) || (static_cast<int>(trade::Status::Unknown) < static_cast<int>(pOrder->m_status)))
			{
				throw std::runtime_error("Broker client ID conflicts with persisted order intent");
			}
			CExecutionRecord changed = record;
			changed.m_orderId = pOrder->m_orderId;
			changed.m_filledQuantity = pOrder->m_nFilled;
			changed.m_brokerStatus = pOrder->m_status;
			changed.m_evidenceVersion = view.m_ordersVersion;
			bool terminal = !Active(pOrder->m_status);
			// Terminal evidence must never regress to an active report at the same watermark.
			if ((ExecutionState::TerminalAwaitingSync == record.m_state) && !terminal)
			{
				continue;
			}
			std::uint64_t resourceVersion = OrderSide::Buy == record.m_intent.m_side ? view.m_fundsVersion : view.m_positionsVersion;
			changed.m_bBrokerCovered = (0 != view.m_ordersVersion) && (resourceVersion >= view.m_ordersVersion);
			changed.m_state = terminal ? (changed.m_bBrokerCovered ? ExecutionState::Closed : ExecutionState::TerminalAwaitingSync) : ExecutionState::BrokerActive;
			changed.m_message = terminal ? (changed.m_bBrokerCovered ? "Terminal order reconciled" : "Terminal order; waiting for resource watermark") : "Broker order reconciled";
			if ((changed.m_state != record.m_state) || (changed.m_evidenceVersion != record.m_evidenceVersion) || (changed.m_bBrokerCovered != record.m_bBrokerCovered) || (changed.m_orderId != record.m_orderId) || (changed.m_filledQuantity != record.m_filledQuantity) || (changed.m_brokerStatus != record.m_brokerStatus))
			{
				Save(changed);
				record = std::move(changed);
			}
		}
		std::erase_if(m_records, [](const auto& item)
		{
			return !IsUnresolved(item.second.m_state);
		});
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

std::string CStrategyExecution::GetLastError() const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	return m_strLastError;
}

std::vector<CExecutionRecord> CStrategyExecution::GetRecords() const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	std::vector<CExecutionRecord> records;
	records.reserve(m_records.size());
	for (const auto& [id, record] : m_records)
	{
		records.emplace_back(record);
	}
	return records;
}

bool CStrategyExecution::HasUnresolved(_TyStrategyId strategyId) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		return true;
	}
	return std::any_of(m_records.begin(), m_records.end(), [strategyId](const auto& item)
	{
		return (strategyId == item.second.m_intent.m_id) && IsUnresolved(item.second.m_state);
	}) || std::any_of(m_locks.begin(), m_locks.end(), [strategyId](const auto& item)
	{
		return strategyId == std::get<2>(item.first);
	});
}

bool CStrategyExecution::IsBuyLockedWithoutLock(const _TyAccount& account, const CSecurity& security) const
{
	std::string key = security.String();
	return std::any_of(m_locks.begin(), m_locks.end(), [&account, &key](const auto& item)
	{
		return (account == std::get<0>(item.first)) && (key == std::get<1>(item.first));
	});
}

bool CStrategyExecution::IsBuyLocked(const _TyAccount& account, const CSecurity& security) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	return !m_bHealthy || IsBuyLockedWithoutLock(account, security);
}

bool CStrategyExecution::LockBuys(const _TyAccount& account, const CSecurity& security, _TyStrategyId ownerId, const std::string& reason)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy || !IsAccountValid(account) || !security.IsValid() || (0 == ownerId) || (ownerId > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) || reason.empty())
	{
		return false;
	}
	try
	{
		_TyLockKey key{ account, security.String(), ownerId };
		_TyStatement statement = Statement(m_database, "INSERT OR IGNORE INTO protection(account,security,owner_id,reason) VALUES(?,?,?,?)");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(ownerId));
		Bind(statement.get(), 4, reason);
		Done(m_database, statement.get());
		m_locks.try_emplace(std::move(key), reason);
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

bool CStrategyExecution::UnlockBuys(const _TyAccount& account, const CSecurity& security, _TyStrategyId ownerId)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		return false;
	}
	for (const auto& [id, record] : m_records)
	{
		if (IsUnresolved(record.m_state) && (account == record.m_intent.m_account) && (security == record.m_intent.m_security))
		{
			return false;
		}
	}
	if (m_exitOwners.end() != m_exitOwners.find(std::make_pair(account, security.String())))
	{
		return false;
	}
	try
	{
		_TyStatement statement = Statement(m_database, "DELETE FROM protection WHERE account=? AND security=? AND owner_id=?");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(ownerId));
		Done(m_database, statement.get());
		m_locks.erase(_TyLockKey{ account, security.String(), ownerId });
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

std::uint64_t CStrategyExecution::AllocateSignal()
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		return 0;
	}
	try
	{
		Execute(m_database, "INSERT INTO signal_sequence DEFAULT VALUES");
		return sqlite3_last_insert_rowid(m_database);
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return 0;
	}
}

std::optional<CExecutionRecord> CStrategyExecution::FindExecution(std::uint64_t clientId) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		return std::nullopt;
	}
	_TyStatement statement = Statement(m_database, "SELECT state,order_id,message,filled,broker_status FROM execution WHERE client_id=?");
	Bind(statement.get(), 1, static_cast<std::int64_t>(clientId));
	int result = sqlite3_step(statement.get());
	if (SQLITE_DONE == result)
	{
		return std::nullopt;
	}
	if (SQLITE_ROW != result)
	{
		throw std::runtime_error(sqlite3_errmsg(m_database));
	}
	CExecutionRecord record;
	record.m_intent.m_clientOrderId = clientId;
	record.m_state = static_cast<ExecutionState>(sqlite3_column_int(statement.get(), 0));
	record.m_orderId = sqlite3_column_int64(statement.get(), 1);
	record.m_message = ReadText(statement.get(), 2);
	record.m_filledQuantity = sqlite3_column_int64(statement.get(), 3);
	record.m_brokerStatus = static_cast<trade::Status>(sqlite3_column_int(statement.get(), 4));
	return record;
}

std::string CStrategyExecution::LoadCheckpoint(_TyStrategyId id) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		throw std::runtime_error("Execution journal is unavailable");
	}
	_TyStatement statement = Statement(m_database, "SELECT data FROM checkpoint WHERE strategy_id=?");
	Bind(statement.get(), 1, static_cast<std::int64_t>(id));
	int result = sqlite3_step(statement.get());
	if (SQLITE_DONE == result)
	{
		return {};
	}
	if (SQLITE_ROW != result)
	{
		throw std::runtime_error(sqlite3_errmsg(m_database));
	}
	return ReadText(statement.get(), 0);
}

bool CStrategyExecution::SaveCheckpoint(_TyStrategyId id, const _TyAccount& account, const CSecurity& security, const std::string& data, bool protect)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy || (0 == id) || !IsAccountValid(account) || !security.IsValid())
	{
		return false;
	}
	try
	{
		Execute(m_database, "BEGIN IMMEDIATE");
		_TyStatement statement = Statement(m_database, "INSERT INTO checkpoint(strategy_id,data) VALUES(?,?) ON CONFLICT(strategy_id) DO UPDATE SET data=excluded.data");
		Bind(statement.get(), 1, static_cast<std::int64_t>(id));
		Bind(statement.get(), 2, data);
		Done(m_database, statement.get());
		if (protect)
		{
			statement = Statement(m_database, "INSERT OR IGNORE INTO protection(account,security,owner_id,reason) VALUES(?,?,?,'Protection exit')");
			Bind(statement.get(), 1, EncodeAccount(account));
			Bind(statement.get(), 2, security.String());
			Bind(statement.get(), 3, static_cast<std::int64_t>(id));
			Done(m_database, statement.get());
		}
		Execute(m_database, "COMMIT");
		if (protect)
		{
			m_locks.try_emplace(_TyLockKey{ account, security.String(), id }, "Protection exit");
		}
		return true;
	}
	catch (const std::exception& error)
	{
		sqlite3_exec(m_database, "ROLLBACK", nullptr, nullptr, nullptr);
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

bool CStrategyExecution::ClaimExit(const _TyAccount& account, const CSecurity& security, _TyStrategyId id)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy || !m_locks.contains(_TyLockKey{ account, security.String(), id }))
	{
		return false;
	}
	const auto mIter = m_exitOwners.find({ account, security.String() });
	if (m_exitOwners.end() != mIter)
	{
		return id == mIter->second;
	}
	try
	{
		_TyStatement statement = Statement(m_database, "INSERT INTO exit_owner(account,security,owner_id) VALUES(?,?,?)");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(id));
		Done(m_database, statement.get());
		m_exitOwners.emplace(std::make_pair(account, security.String()), id);
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

bool CStrategyExecution::ReleaseExit(const _TyAccount& account, const CSecurity& security, _TyStrategyId id)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		return false;
	}
	for (const auto& [clientId, record] : m_records)
	{
		if ((account == record.m_intent.m_account) && (security == record.m_intent.m_security))
		{
			return false;
		}
	}
	try
	{
		_TyStatement statement = Statement(m_database, "DELETE FROM exit_owner WHERE account=? AND security=? AND owner_id=?");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(id));
		Done(m_database, statement.get());
		const auto mIter = m_exitOwners.find({ account, security.String() });
		if ((m_exitOwners.end() != mIter) && (id == mIter->second))
		{
			m_exitOwners.erase(mIter);
		}
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}

std::int64_t CStrategyExecution::AvailableFunds(const _TyAccount& account, const CStrategyAccountView& view) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy || (account != view.m_identity) || !view.m_account.m_bReady)
	{
		return 0;
	}
	std::int64_t available = view.m_account.m_available;
	for (const auto& [id, record] : m_records)
	{
		if ((account == record.m_intent.m_account) && (OrderSide::Buy == record.m_intent.m_side) && !record.m_bBrokerCovered)
		{
			if (available < record.m_reservedMoney)
			{
				return 0;
			}
			available -= record.m_reservedMoney;
		}
	}
	return available;
}

std::optional<CExecutionRecord> CStrategyExecution::FindSignal(_TyStrategyId id, std::uint64_t signalId) const
{
	std::uint64_t client = 0;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (!m_bHealthy)
		{
			return std::nullopt;
		}
		_TyStatement statement = Statement(m_database, "SELECT client_id FROM execution WHERE strategy_id=? AND signal_id=?");
		Bind(statement.get(), 1, static_cast<std::int64_t>(id));
		Bind(statement.get(), 2, static_cast<std::int64_t>(signalId));
		int result = sqlite3_step(statement.get());
		if (SQLITE_DONE == result)
		{
			return std::nullopt;
		}
		if (SQLITE_ROW != result)
		{
			throw std::runtime_error(sqlite3_errmsg(m_database));
		}
		client = sqlite3_column_int64(statement.get(), 0);
	}
	return FindExecution(client);
}

bool CStrategyExecution::RejectBeforeAcceptance(std::uint64_t clientId, const std::string& reason)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	const auto mIter = m_records.find(clientId);
	if (!m_bHealthy || (m_records.end() == mIter) || (0 != mIter->second.m_orderId))
	{
		return false;
	}
	try
	{
		CExecutionRecord changed = mIter->second;
		changed.m_state = ExecutionState::Closed;
		changed.m_brokerStatus = trade::Status::Rejected;
		changed.m_message = reason;
		Save(changed);
		m_records.erase(mIter);
		return true;
	}
	catch (const std::exception& error)
	{
		m_bHealthy = false;
		m_strLastError = error.what();
		return false;
	}
}
