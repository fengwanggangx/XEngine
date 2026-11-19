#include "CStrategyExecution.h"

#include "../database/IDataBase.h"
#include <concepts>
#include <iterator>
#include <type_traits>
#include "../common/utility.h"

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>

// A retained pool lease pins transactions and the named lock to one MySQL session.
class CExecutionDatabase final
{
public:
	explicit CExecutionDatabase(db::_TyDBPtr&& connection) : m_connection(std::move(connection))
	{
		try
		{
			if (nullptr == m_connection)
			{
				throw std::runtime_error("MySQL connection unavailable");
			}
			const db::CTable& table = m_connection->ExecQuery("SELECT CONNECTION_ID(),CONCAT('xengine.strategy.',LEFT(SHA2(DATABASE(),256),40)),@@global.innodb_flush_log_at_trx_commit,@@global.log_bin,@@global.sync_binlog");
			if ((1 != table.m_rows.size()) || (5 != table.m_columns.size()) || !utility::to_number(db::to_string(table.m_rows.front()[0]), m_sessionId) || (0 >= m_sessionId) || ("1" != db::to_string(table.m_rows.front()[2])) || (("1" == db::to_string(table.m_rows.front()[3])) && ("1" != db::to_string(table.m_rows.front()[4]))))
			{
				throw std::runtime_error("MySQL requires a stable session, innodb_flush_log_at_trx_commit=1 and sync_binlog=1 when binary logging is enabled");
			}
			m_lockName = db::to_string(table.m_rows.front()[1]);
			const db::CTable& locked = m_connection->ExecQuery("SELECT GET_LOCK(" + utility::Utf8Literal(m_lockName) + ",0)");
			if ((1 != locked.m_rows.size()) || (1 != locked.m_columns.size()) || ("1" != db::to_string(locked.m_rows.front()[0])))
			{
				throw std::runtime_error("Another engine owns MySQL strategy execution");
			}
			CheckOwner();
		}
		catch (...)
		{
			if (nullptr != m_connection)
			{
				m_connection->Close();
			}
			throw;
		}
	}
	~CExecutionDatabase()
	{
		if (nullptr != m_connection)
		{
			try
			{
				if (m_inTransaction)
				{
					m_connection->RollBackTransaction();
				}
				CheckOwner();
				const db::CTable& released = m_connection->ExecQuery("SELECT RELEASE_LOCK(" + utility::Utf8Literal(m_lockName) + ")");
				if ((1 != released.m_rows.size()) || ("1" != db::to_string(released.m_rows.front()[0])))
				{
					m_connection->Close();
				}
			}
			catch (...)
			{
				m_connection->Close();
			}
		}
	}
	void CheckOwner()
	{
		const db::CTable& table = m_connection->ExecQuery("SELECT CONNECTION_ID(),IS_USED_LOCK(" + utility::Utf8Literal(m_lockName) + ")");
		std::int64_t session = 0, owner = 0;
		if ((1 != table.m_rows.size()) || (2 != table.m_columns.size()) || !utility::to_number(db::to_string(table.m_rows.front()[0]), session) || !utility::to_number(db::to_string(table.m_rows.front()[1]), owner) || (m_sessionId != session) || (m_sessionId != owner))
		{
			throw std::runtime_error("MySQL strategy execution session or ownership lost; restart and reconcile");
		}
	}
	db::CTable Query(const std::string& sql)
	{
		CheckOwner();
		const db::CTable& table = m_connection->ExecQuery(sql);
		if (table.m_columns.empty())
		{
			throw std::runtime_error("MySQL strategy execution query failed");
		}
		return table;
	}
	void Execute(const std::string& sql)
	{
		CheckOwner();
		if ("START TRANSACTION" == sql)
		{
			if (m_inTransaction || !m_connection->BeginTransaction())
			{
				throw std::runtime_error("Cannot begin MySQL execution transaction");
			}
			m_inTransaction = true;
			return;
		}
		if ("COMMIT" == sql)
		{
			if (!m_inTransaction || !m_connection->EndTransaction())
			{
				throw std::runtime_error("MySQL execution commit result unknown; do not send");
			}
			m_inTransaction = false;
			return;
		}
		if ("ROLLBACK" == sql)
		{
			if (!m_connection->RollBackTransaction())
			{
				throw std::runtime_error("MySQL execution rollback failed");
			}
			m_inTransaction = false;
			return;
		}
		if (0 != m_connection->ExecUpdate(sql))
		{
			throw std::runtime_error("MySQL strategy execution write failed");
		}
	}
	void Rollback() noexcept
	{
		if (m_inTransaction)
		{
			m_connection->RollBackTransaction();
			m_inTransaction = false;
		}
	}
	std::int64_t Scalar(const std::string& sql)
	{
		db::CTable table = Query(sql);
		std::int64_t value = 0;
		if ((1 != table.m_rows.size()) || (1 != table.m_columns.size()) || !utility::to_number(db::to_string(table.m_rows.front()[0]), value))
		{
			throw std::runtime_error("Invalid MySQL execution scalar");
		}
		return value;
	}

private:
	db::_TyDBPtr m_connection;
	std::int64_t m_sessionId{ 0 };
	std::string m_lockName;
	bool m_inTransaction{ false };
};

namespace
{
	enum class StatementResult
	{
		Row,
		Done
	};
	class CStatement final
	{
	public:
		CStatement(CExecutionDatabase* database, const std::string& sql) : m_database(database), m_sql(sql)
		{
		}
		void Bind(int index, const std::string& literal)
		{
			if (index != static_cast<int>(m_values.size()) + 1)
			{
				throw std::runtime_error("Invalid execution SQL binding order");
			}
			m_values.emplace_back(literal);
		}
		std::string Sql() const
		{
			std::string sql;
			std::size_t index = 0;
			for (char value : m_sql)
			{
				if ('?' == value)
				{
					if (index >= m_values.size())
					{
						throw std::runtime_error("Missing execution SQL binding");
					}
					sql += m_values[index++];
				}
				else
				{
					sql += value;
				}
			}
			if (index != m_values.size())
			{
				throw std::runtime_error("Unused execution SQL binding");
			}
			return sql;
		}
		StatementResult Step()
		{
			if (!m_queried)
			{
				m_table = m_database->Query(Sql());
				m_queried = true;
			}
			if (m_cursor >= m_table.m_rows.size())
			{
				return StatementResult::Done;
			}
			++m_cursor;
			return StatementResult::Row;
		}
		std::string Text(int index) const
		{
			if ((0 == m_cursor) || (0 > index) || (static_cast<std::size_t>(index) >= m_table.m_rows[m_cursor - 1].size()))
			{
				throw std::runtime_error("Invalid execution SQL result");
			}
			return db::to_string(m_table.m_rows[m_cursor - 1][index]);
		}

	private:
		CExecutionDatabase* m_database;
		std::string m_sql;
		std::vector<std::string> m_values;
		db::CTable m_table;
		std::size_t m_cursor{ 0 };
		bool m_queried{ false };
	};
	using _TyStatement = std::unique_ptr<CStatement>;
	void Execute(CExecutionDatabase* database, const char* sql)
	{
		database->Execute(sql);
	}
	_TyStatement Statement(CExecutionDatabase* database, const char* sql)
	{
		return std::make_unique<CStatement>(database, sql);
	}
	void Bind(CStatement* statement, int index, std::int64_t value)
	{
		statement->Bind(index, std::to_string(value));
	}
	void Bind(CStatement* statement, int index, const std::string& value)
	{
		statement->Bind(index, utility::Utf8Literal(value));
	}
	std::string ReadText(CStatement* statement, int index)
	{
		return statement->Text(index);
	}
	std::int64_t ReadInteger(CStatement* statement, int index)
	{
		std::int64_t value = 0;
		if (!utility::to_number(statement->Text(index), value))
		{
			throw std::runtime_error("Invalid execution SQL integer");
		}
		return value;
	}
	StatementResult Step(CStatement* statement)
	{
		return statement->Step();
	}
	void Done(CExecutionDatabase* database, CStatement* statement)
	{
		database->Execute(statement->Sql());
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

CStrategyExecution::CStrategyExecution() = default;
CStrategyExecution::~CStrategyExecution() = default;

bool CStrategyExecution::Open(db::_TyDBPtr connection)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (nullptr != m_database)
	{
		m_strLastError = "Execution store is already open";
		return false;
	}
	try
	{
		m_database = std::make_unique<CExecutionDatabase>(std::move(connection));
		db::CTable tables = m_database->Query("SELECT TABLE_NAME,ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('table_strategy_execution','table_strategy_protection','table_strategy_checkpoint','table_strategy_exit_owner','table_strategy_signal_sequence')");
		if (5 != tables.m_rows.size())
		{
			throw std::runtime_error("MySQL strategy execution tables are missing");
		}
		for (const auto& row : tables.m_rows)
		{
			if ("InnoDB" != db::to_string(row[1]))
			{
				throw std::runtime_error("Strategy execution tables require InnoDB");
			}
		}
		m_database->Execute("START TRANSACTION");
		Load();
		m_database->Execute("UPDATE table_strategy_execution SET state=2,covered=0,evidence=0,message='Restarted; reconcile existing order before retry' WHERE state NOT IN (5,6)");
		m_database->Execute("COMMIT");
		m_bHealthy = true;
		m_strLastError.clear();
		return true;
	}
	catch (const std::exception& error)
	{
		m_strLastError = error.what();
		m_database.reset();
		m_records.clear();
		m_locks.clear();
		m_exitOwners.clear();
		return false;
	}
}

void CStrategyExecution::Load()
{
	_TyStatement statement = Statement(m_database.get(), "SELECT client_id,account,strategy_id,signal_id,security,exchange_id,side,price,price_scale,quantity,effect,state,reserved_money,order_id,evidence,covered,message FROM table_strategy_execution WHERE state NOT IN (5,6)");
	StatementResult result = StatementResult::Row;
	while (StatementResult::Row == (result = Step(statement.get())))
	{
		CExecutionRecord record;
		COrderIntent& intent = record.m_intent;
		intent.m_clientOrderId = ReadInteger(statement.get(), 0);
		if (!DecodeAccount(ReadText(statement.get(), 1), intent.m_account))
		{
			throw std::runtime_error("Invalid account in strategy journal");
		}
		intent.m_id = ReadInteger(statement.get(), 2);
		intent.m_signalId = ReadInteger(statement.get(), 3);
		intent.m_security = CSecurity(ReadText(statement.get(), 4), static_cast<Exchange>(ReadInteger(statement.get(), 5)));
		intent.m_side = static_cast<OrderSide>(ReadInteger(statement.get(), 6));
		intent.m_price = ReadInteger(statement.get(), 7);
		intent.m_nPriceScale = static_cast<int>(ReadInteger(statement.get(), 8));
		intent.m_quantity = ReadInteger(statement.get(), 9);
		intent.m_positionEffect = static_cast<PositionEffect>(ReadInteger(statement.get(), 10));
		std::int64_t state = ReadInteger(statement.get(), 11);
		if ((0 > state) || (4 < state) || !ValidIntent(intent) || (0 == intent.m_clientOrderId))
		{
			throw std::runtime_error("Invalid execution in strategy journal");
		}
		// A crash can occur at any point after preparation. Never assume not sent.
		record.m_state = ExecutionState::Unknown;
		record.m_reservedMoney = ReadInteger(statement.get(), 12);
		record.m_orderId = ReadInteger(statement.get(), 13);
		record.m_evidenceVersion = 0;
		record.m_bBrokerCovered = false;
		record.m_message = "Restarted; reconcile existing order before retry";
		if (0 > record.m_reservedMoney)
		{
			throw std::runtime_error("Invalid reservation in strategy journal");
		}
		m_records.emplace(intent.m_clientOrderId, std::move(record));
	}
	if (StatementResult::Done != result)
	{
		throw std::runtime_error("MySQL execution operation failed");
	}
	statement = Statement(m_database.get(), "SELECT account,security,owner_id,reason FROM table_strategy_protection");
	while (StatementResult::Row == (result = Step(statement.get())))
	{
		_TyAccount account;
		std::int64_t owner = ReadInteger(statement.get(), 2);
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
	if (StatementResult::Done != result)
	{
		throw std::runtime_error("MySQL execution operation failed");
	}
	statement = Statement(m_database.get(), "SELECT account,security,owner_id FROM table_strategy_exit_owner");
	while (StatementResult::Row == (result = Step(statement.get())))
	{
		_TyAccount account;
		if (!DecodeAccount(ReadText(statement.get(), 0), account) || (0 >= ReadInteger(statement.get(), 2)))
		{
			throw std::runtime_error("Invalid exit owner");
		}
		_TyStrategyId ownerId = static_cast<_TyStrategyId>(ReadInteger(statement.get(), 2));
		std::string security = ReadText(statement.get(), 1);
		if (m_locks.end() == m_locks.find(_TyLockKey{ account, security, ownerId }))
		{
			throw std::runtime_error("Exit owner has no protection lock");
		}
		m_exitOwners.emplace(std::make_pair(account, security), ownerId);
	}
	if (StatementResult::Done != result)
	{
		throw std::runtime_error("MySQL execution operation failed");
	}
}

void CStrategyExecution::Save(const CExecutionRecord& record)
{
	_TyStatement statement = Statement(m_database.get(), "UPDATE table_strategy_execution SET state=?,order_id=?,evidence=?,covered=?,message=?,filled=?,broker_status=? WHERE client_id=?");
	Bind(statement.get(), 1, static_cast<std::int64_t>(record.m_state));
	Bind(statement.get(), 2, static_cast<std::int64_t>(record.m_orderId));
	Bind(statement.get(), 3, static_cast<std::int64_t>(record.m_evidenceVersion));
	Bind(statement.get(), 4, static_cast<std::int64_t>(record.m_bBrokerCovered));
	Bind(statement.get(), 5, record.m_message);
	Bind(statement.get(), 6, record.m_filledQuantity);
	Bind(statement.get(), 7, static_cast<std::int64_t>(record.m_brokerStatus));
	Bind(statement.get(), 8, static_cast<std::int64_t>(record.m_intent.m_clientOrderId));
	Done(m_database.get(), statement.get());
	// Owner checks execute SELECTs; verify existence directly instead of relying on session ROW_COUNT().
	if (1 != m_database->Scalar("SELECT COUNT(*) FROM table_strategy_execution WHERE client_id=" + std::to_string(record.m_intent.m_clientOrderId)))
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
		Execute(m_database.get(), "START TRANSACTION");
		_TyStatement duplicate = Statement(m_database.get(), "SELECT client_id FROM table_strategy_execution WHERE account=? AND strategy_id=? AND signal_id=?");
		Bind(duplicate.get(), 1, EncodeAccount(intent.m_account));
		Bind(duplicate.get(), 2, static_cast<std::int64_t>(intent.m_id));
		Bind(duplicate.get(), 3, static_cast<std::int64_t>(intent.m_signalId));
		if (StatementResult::Row == Step(duplicate.get()))
		{
			Execute(m_database.get(), "ROLLBACK");
			return { false, false, 0, 0, "Duplicate execution signal; reconcile the original order" };
		}

		_TyStatement statement = Statement(m_database.get(), "INSERT INTO table_strategy_execution(account,strategy_id,signal_id,security,exchange_id,side,price,price_scale,quantity,effect,state,reserved_money,order_id,evidence,covered,message) VALUES(?,?,?,?,?,?,?,?,?,?,0,?,0,0,0,'Prepared before send')");
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
		Done(m_database.get(), statement.get());
		intent.m_clientOrderId = m_database->Scalar("SELECT LAST_INSERT_ID()");
		CExecutionRecord record;
		record.m_intent = intent;
		record.m_reservedMoney = maximumDebit;
		record.m_message = "Prepared before send";
		// Allocate before commit so allocation failure cannot lose a durable hold.
		m_records.emplace(intent.m_clientOrderId, std::move(record));
		Execute(m_database.get(), "COMMIT");
		return { true, false, intent.m_clientOrderId, 0, "Execution persisted; ready to send" };
	}
	catch (const std::exception& error)
	{
		m_database->Rollback();
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
			_TyStatement statement = Statement(m_database.get(), "SELECT state FROM table_strategy_execution WHERE client_id=?");
			Bind(statement.get(), 1, static_cast<std::int64_t>(clientId));
			StatementResult result = Step(statement.get());
			if ((StatementResult::Row != result) && (StatementResult::Done != result))
			{
				throw std::runtime_error("MySQL execution operation failed");
			}
			return (StatementResult::Row == result) && ((static_cast<int>(ExecutionState::Closed) == ReadInteger(statement.get(), 0)) || (static_cast<int>(ExecutionState::NotSent) == ReadInteger(statement.get(), 0)));
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
		_TyStatement statement = Statement(m_database.get(), "INSERT INTO table_strategy_protection(account,security,owner_id,reason) VALUES(?,?,?,?) ON DUPLICATE KEY UPDATE owner_id=owner_id");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(ownerId));
		Bind(statement.get(), 4, reason);
		Done(m_database.get(), statement.get());
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
		_TyStatement statement = Statement(m_database.get(), "DELETE FROM table_strategy_protection WHERE account=? AND security=? AND owner_id=?");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(ownerId));
		Done(m_database.get(), statement.get());
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
		Execute(m_database.get(), "INSERT INTO table_strategy_signal_sequence() VALUES()");
		return m_database->Scalar("SELECT LAST_INSERT_ID()");
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
	_TyStatement statement = Statement(m_database.get(), "SELECT state,order_id,message,filled,broker_status FROM table_strategy_execution WHERE client_id=?");
	Bind(statement.get(), 1, static_cast<std::int64_t>(clientId));
	StatementResult result = Step(statement.get());
	if (StatementResult::Done == result)
	{
		return std::nullopt;
	}
	if (StatementResult::Row != result)
	{
		throw std::runtime_error("MySQL execution operation failed");
	}
	CExecutionRecord record;
	record.m_intent.m_clientOrderId = clientId;
	record.m_state = static_cast<ExecutionState>(ReadInteger(statement.get(), 0));
	record.m_orderId = ReadInteger(statement.get(), 1);
	record.m_message = ReadText(statement.get(), 2);
	record.m_filledQuantity = ReadInteger(statement.get(), 3);
	record.m_brokerStatus = static_cast<trade::Status>(ReadInteger(statement.get(), 4));
	return record;
}

std::string CStrategyExecution::LoadCheckpoint(_TyStrategyId id, const std::string& key) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy)
	{
		throw std::runtime_error("Execution journal is unavailable");
	}
	_TyStatement statement = Statement(m_database.get(), key.empty() ? "SELECT data FROM table_strategy_checkpoint WHERE strategy_id=?" : "SELECT data FROM table_strategy_security_checkpoint WHERE strategy_id=? AND security=?");
	Bind(statement.get(), 1, static_cast<std::int64_t>(id));
	if (!key.empty())
	{
		Bind(statement.get(), 2, key);
	}
	StatementResult result = Step(statement.get());
	if (StatementResult::Done == result)
	{
		return {};
	}
	if (StatementResult::Row != result)
	{
		throw std::runtime_error("MySQL execution operation failed");
	}
	return ReadText(statement.get(), 0);
}

bool CStrategyExecution::SaveCheckpoint(_TyStrategyId id, const _TyAccount& account, const CSecurity& security, const std::string& data, bool protect, bool scoped)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_bHealthy || (0 == id) || !IsAccountValid(account) || !security.IsValid())
	{
		return false;
	}
	try
	{
		Execute(m_database.get(), "START TRANSACTION");
		_TyStatement statement = Statement(m_database.get(), scoped ? "INSERT INTO table_strategy_security_checkpoint(strategy_id,data,security) VALUES(?,?,?) ON DUPLICATE KEY UPDATE data=VALUES(data)" : "INSERT INTO table_strategy_checkpoint(strategy_id,data) VALUES(?,?) ON DUPLICATE KEY UPDATE data=VALUES(data)");
		Bind(statement.get(), 1, static_cast<std::int64_t>(id));
		Bind(statement.get(), 2, data);
		if (scoped)
		{
			Bind(statement.get(), 3, security.String());
		}
		Done(m_database.get(), statement.get());
		if (protect)
		{
			statement = Statement(m_database.get(), "INSERT INTO table_strategy_protection(account,security,owner_id,reason) VALUES(?,?,?,'Protection exit') ON DUPLICATE KEY UPDATE owner_id=owner_id");
			Bind(statement.get(), 1, EncodeAccount(account));
			Bind(statement.get(), 2, security.String());
			Bind(statement.get(), 3, static_cast<std::int64_t>(id));
			Done(m_database.get(), statement.get());
		}
		Execute(m_database.get(), "COMMIT");
		if (protect)
		{
			m_locks.try_emplace(_TyLockKey{ account, security.String(), id }, "Protection exit");
		}
		return true;
	}
	catch (const std::exception& error)
	{
		m_database->Rollback();
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
		_TyStatement statement = Statement(m_database.get(), "INSERT INTO table_strategy_exit_owner(account,security,owner_id) VALUES(?,?,?)");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(id));
		Done(m_database.get(), statement.get());
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
		_TyStatement statement = Statement(m_database.get(), "DELETE FROM table_strategy_exit_owner WHERE account=? AND security=? AND owner_id=?");
		Bind(statement.get(), 1, EncodeAccount(account));
		Bind(statement.get(), 2, security.String());
		Bind(statement.get(), 3, static_cast<std::int64_t>(id));
		Done(m_database.get(), statement.get());
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
		_TyStatement statement = Statement(m_database.get(), "SELECT client_id FROM table_strategy_execution WHERE strategy_id=? AND signal_id=?");
		Bind(statement.get(), 1, static_cast<std::int64_t>(id));
		Bind(statement.get(), 2, static_cast<std::int64_t>(signalId));
		StatementResult result = Step(statement.get());
		if (StatementResult::Done == result)
		{
			return std::nullopt;
		}
		if (StatementResult::Row != result)
		{
			throw std::runtime_error("MySQL execution operation failed");
		}
		client = ReadInteger(statement.get(), 0);
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
