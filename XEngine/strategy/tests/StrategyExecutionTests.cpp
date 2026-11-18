#include "../CStrategyExecution.h"
#include "MySqlTestDatabase.h"
#include "../../database/IDataBase.h"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
	void Check(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	_TyAccount Account()
	{
		return { 1, "000123" };
	}

	CSecurity Security(const std::string& code = "600000")
	{
		return CSecurity(code, Exchange::sse);
	}

	trade::CAccountData Data()
	{
		trade::CAccountData data;
		data.m_bReady = true;
		data.m_version = 10;
		data.m_fundsVersion = 10;
		data.m_positionsVersion = 10;
		data.m_ordersVersion = 10;
		data.m_account.m_account = Account();
		data.m_account.m_bConnected = true;
		data.m_account.m_nBalance = 200000;
		data.m_account.m_nAvailable = 200000;
		data.m_account.m_bExplicitAvailable = true;
		trade::CPosition position;
		position.m_account = Account();
		position.m_strSecurity = Security().String();
		position.m_nTotal = 300;
		position.m_nAvailable = 100;
		position.m_nToday = 200;
		position.m_nCost = 100001;
		data.m_positions.emplace_back(position);
		return data;
	}

	COrderIntent Intent(std::uint64_t signal = 1, const std::string& code = "600000")
	{
		COrderIntent intent;
		intent.m_account = Account();
		intent.m_id = 42;
		intent.m_signalId = signal;
		intent.m_security = Security(code);
		intent.m_side = OrderSide::Buy;
		intent.m_positionEffect = PositionEffect::Open;
		intent.m_price = 1000;
		intent.m_quantity = 100;
		return intent;
	}

	trade::COrder BrokerOrder(const CExecutionRecord& record, trade::Status status)
	{
		trade::COrder order;
		order.m_account = record.m_intent.m_account;
		order.m_clientId = record.m_intent.m_clientOrderId;
		order.m_id = record.m_intent.m_id;
		order.m_signalId = record.m_intent.m_signalId;
		order.m_orderId = order.m_clientId + 1;
		order.m_strSecurity = record.m_intent.m_security.String();
		order.m_side = OrderSide::Buy == record.m_intent.m_side ? trade::Side::Buy : trade::Side::Sell;
		order.m_nPrice = record.m_intent.m_price;
		order.m_nPriceScale = record.m_intent.m_nPriceScale;
		order.m_nQuantity = record.m_intent.m_quantity;
		order.m_status = status;
		return order;
	}

	void TestSnapshot()
	{
		trade::CAccountData data = Data();
		CStrategyAccountView view = MakeStrategyAccountView(data, Security());
		Check(view.m_account.m_bReady && view.m_position.m_bReady, "ready snapshot");
		Check(view.m_account.m_version == view.m_position.m_version, "one version for resources");
		Check(100001 == view.m_position.m_totalCost, "preserve total cost remainder");
		Check(view.m_position.m_bCostKnown, "cost validity is propagated");
		data.m_positions.front().m_bCostKnown = false;
		view = MakeStrategyAccountView(data, Security());
		Check(!view.m_position.m_bCostKnown && 0 == view.m_position.m_averageCost, "unknown cost is not fabricated");
		data.m_positions.front().m_nAvailable = 301;
		Check(!MakeStrategyAccountView(data, Security()).m_position.m_bReady, "reject invalid quantities");
		data = Data();
		data.m_positions.emplace_back(data.m_positions.front());
		Check(!MakeStrategyAccountView(data, Security()).m_position.m_bReady, "reject duplicate position rows");
		data = Data();
		data.m_account.m_bExplicitAvailable = false;
		data.m_account.m_bFrozenKnown = false;
		Check(!MakeStrategyAccountView(data, Security()).m_account.m_bReady, "no derived funds with unknown frozen amount");
		data = Data();
		data.m_bReady = false;
		Check(!MakeStrategyAccountView(data, Security()).m_position.m_bReady, "unready data cannot trade");
	}

	void TestAdmission()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open in-memory journal");
		trade::CAccountData data = Data();
		COrderSubmitResult first = journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500);
		Check(first.m_bAccepted && 0 != first.m_clientOrderId, "prepare before transport");
		Check(!journal.Prepare(Intent(2), MakeStrategyAccountView(data, Security()), 100500).m_bAccepted, "one execution per security");
		Check(!journal.Prepare(Intent(3, "600001"), MakeStrategyAccountView(data, Security("600001")), 100500).m_bAccepted, "cross-security account funds reservation");
		Check(!journal.Prepare(Intent(4, "600002"), MakeStrategyAccountView(data, Security("600002")), 99999).m_bAccepted, "budget must cover principal");
		COrderIntent mismatched = Intent(5, "600003");
		mismatched.m_account = { 2, "000123" };
		Check(!journal.Prepare(mismatched, MakeStrategyAccountView(data, Security("600003")), 100500).m_bAccepted, "snapshot cannot cross accounts");
		Check(journal.MarkSubmission(first.m_clientOrderId, false, true), "definitely unsent can release");
		Check(!journal.HasUnresolved(42), "unsent is terminal");
		Check(!journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500).m_bAccepted, "signal stays deduplicated after release");
		COrderIntent sell = Intent(6);
		sell.m_side = OrderSide::Sell;
		sell.m_positionEffect = PositionEffect::Close;
		sell.m_quantity = 200;
		Check(!journal.Prepare(sell, MakeStrategyAccountView(data, Security()), 0).m_bAccepted, "cannot sell today's unavailable shares");
		sell.m_quantity = 100;
		Check(journal.Prepare(sell, MakeStrategyAccountView(data, Security()), 0).m_bAccepted, "sell available quantity");
	}

	void TestWatermarks()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open watermark journal");
		trade::CAccountData data = Data();
		COrderSubmitResult prepared = journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500);
		Check(prepared.m_bAccepted, "prepare watermark execution");
		Check(journal.MarkSubmission(prepared.m_clientOrderId, true), "mark queued");
		data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Filled));
		data.m_version = 20;
		data.m_ordersVersion = 20;
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "terminal order evidence");
		Check(journal.HasUnresolved(42), "terminal order alone must not release money");
		Check(ExecutionState::TerminalAwaitingSync == journal.GetRecords().front().m_state, "wait for funds watermark");
		data.m_fundsVersion = 20;
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "resource evidence");
		Check(!journal.HasUnresolved(42), "release only after funds catch up");
		Check(journal.MarkSubmission(prepared.m_clientOrderId, true), "late transport completion is harmless");
		Check(journal.GetRecords().empty(), "terminal history does not accumulate in the pending map");
		data.m_version = 10;
		Check(!journal.Prepare(Intent(8, "600008"), MakeStrategyAccountView(data, Security("600008")), 100500).m_bAccepted, "reject older snapshot after newer evidence");
	}

	void TestHandoff()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open handoff journal");
		trade::CAccountData data = Data();
		Check(journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500).m_bAccepted, "prepare first debit");
		data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Accepted));
		data.m_version = 20;
		data.m_ordersVersion = 20;
		data.m_fundsVersion = 20;
		data.m_account.m_nAvailable = 99499;
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "handoff broker freeze");
		COrderIntent smaller = Intent(2, "600001");
		smaller.m_quantity = 90;
		Check(journal.Prepare(smaller, MakeStrategyAccountView(data, smaller.m_security), 90500).m_bAccepted, "broker frozen funds must not be subtracted twice");
	}

	void TestConcurrentAdmission()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open concurrent journal");
		std::atomic_int accepted{ 0 };
		std::vector<std::jthread> workers;
		for (int index = 0; 12 > index; ++index)
		{
			workers.emplace_back([index, &journal, &accepted]()
			{
				COrderIntent intent = Intent(index + 1, "6000" + std::to_string(index + 10));
				if (journal.Prepare(intent, MakeStrategyAccountView(Data(), intent.m_security), 100500).m_bAccepted)
				{
					++accepted;
				}
			});
		}
		workers.clear();
		Check(1 == accepted.load(), "only one concurrent debit fits the shared balance");
	}

	void TestSellWatermarks()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open sell watermark journal");
		COrderIntent intent = Intent();
		intent.m_side = OrderSide::Sell;
		intent.m_positionEffect = PositionEffect::Close;
		trade::CAccountData data = Data();
		Check(journal.Prepare(intent, MakeStrategyAccountView(data, Security()), 0).m_bAccepted, "prepare sell");
		data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Filled));
		data.m_version = 20;
		data.m_ordersVersion = 20;
		data.m_fundsVersion = 20;
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "observe filled sell");
		Check(journal.HasUnresolved(42), "funds cannot release a sell's share reservation");
		data.m_positionsVersion = 20;
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "observe position watermark");
		Check(!journal.HasUnresolved(42), "position evidence releases the sell");
	}

	void TestMissingCoveredOrder()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open missing-order journal");
		trade::CAccountData data = Data();
		Check(journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500).m_bAccepted, "prepare disappearing order");
		data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Accepted));
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "broker freeze covers reservation");
		Check(journal.GetRecords().front().m_bBrokerCovered, "covered before disappearance");
		data.m_version = 20;
		data.m_orders.clear();
		Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "missing row is uncertain");
		Check(!journal.GetRecords().front().m_bBrokerCovered, "missing row restores local reservation");
		Check(!journal.Prepare(Intent(2, "600001"), MakeStrategyAccountView(data, Security("600001")), 100500).m_bAccepted, "missing evidence cannot make funds available");
	}

	void TestDuplicateBrokerIdentity()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open duplicate-order journal");
		trade::CAccountData data = Data();
		Check(journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500).m_bAccepted, "prepare duplicate identity test");
		data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Filled));
		data.m_orders.emplace_back(data.m_orders.front());
		Check(!journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "duplicate broker identity cannot release an execution");
		Check(journal.HasUnresolved(42), "invalid broker evidence remains blocked");
	}

	void TestRecovery(const std::string& name)
	{
		std::uint64_t firstId = 0;
		{
			CStrategyExecution journal;
			Check(journal.Open(OpenTestDatabase(name)), "open disk journal");
			CStrategyExecution secondOwner;
			Check(!secondOwner.Open(OpenTestDatabase(name)), "reject a second owning engine");
			COrderSubmitResult prepared = journal.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500);
			Check(prepared.m_bAccepted, "persist prepared intent");
			firstId = prepared.m_clientOrderId;
			Check(journal.LockBuys(Account(), Security(), 42, "stop_loss"), "persist protection lock");
			Check(!journal.UnlockBuys(Account(), Security(), 42), "cannot unlock unresolved execution");
		}
		{
			CStrategyExecution journal;
			Check(journal.Open(OpenTestDatabase(name)), "reopen journal after simulated crash");
			Check(ExecutionState::Unknown == journal.GetRecords().front().m_state, "prepared crash window is uncertain");
			Check(firstId == journal.GetRecords().front().m_intent.m_clientOrderId, "keep durable client identity");
			Check(ExecutionState::Unknown == journal.FindExecution(firstId)->m_state, "recovered unknown is durable in MySQL");
			Check(journal.IsBuyLocked(Account(), Security()), "protection lock survives restart");
			Check(journal.Reconcile(Account(), MakeStrategyAccountView(Data(), Security())), "empty broker query is accepted as observation");
			Check(journal.HasUnresolved(42), "absence cannot clear uncertain order");
			Check(journal.MarkSubmission(firstId, false, true), "late unsent assumption cannot rewrite recovered state");
			Check(journal.HasUnresolved(42), "recovered unknown remains blocked");
			trade::CAccountData data = Data();
			data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Filled));
			Check(journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "recover by exact broker identity");
			Check(journal.UnlockBuys(Account(), Security(), 42), "unlock after reconciliation");
			Check(!journal.HasUnresolved(42), "recovered terminal and lock cleared");
			Check(!journal.Prepare(Intent(), MakeStrategyAccountView(data, Security()), 100500).m_bAccepted, "durable unique signal survives restart");
			COrderSubmitResult next = journal.Prepare(Intent(2, "600001"), MakeStrategyAccountView(data, Security("600001")), 100500);
			Check(next.m_bAccepted && firstId < next.m_clientOrderId, "client ID never reused after reopen");
		}
	}

	void TestProtection()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open protection journal");
		Check(journal.LockBuys(Account(), Security(), 42, "stop_loss"), "first protection owner");
		Check(journal.LockBuys(Account(), Security(), 43, "trailing"), "second protection owner");
		Check(journal.UnlockBuys(Account(), Security(), 42), "release own lock");
		Check(journal.IsBuyLocked(Account(), Security()), "other owner still blocks buys");
		Check(!journal.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "locked buy denied");
		Check(journal.UnlockBuys(Account(), Security(), 43), "release last owner");
		Check(journal.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "buy reopens only after all locks release");
	}

	void TestCheckpointOwnership()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open checkpoint journal");
		std::uint64_t first = journal.AllocateSignal();
		Check(0 != first && first < journal.AllocateSignal(), "durable signal sequence increases");
		Check(journal.SaveCheckpoint(42, Account(), Security(), "active-task", true), "checkpoint and protection are atomic");
		Check("active-task" == journal.LoadCheckpoint(42), "load strategy checkpoint");
		Check(journal.ClaimExit(Account(), Security(), 42), "protected task claims symbol exit");
		Check(!journal.UnlockBuys(Account(), Security(), 42), "active exit owner blocks unlock");
		Check(!journal.ClaimExit(Account(), Security(), 43), "other task cannot claim owned exit");
		Check(journal.ReleaseExit(Account(), Security(), 42), "release completed exit ownership");
		Check(journal.UnlockBuys(Account(), Security(), 42), "unlock after ownership released");
	}

	void TestConflict()
	{
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase()), "open conflict journal");
		Check(journal.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "prepare conflict test");
		trade::CAccountData data = Data();
		data.m_orders.emplace_back(BrokerOrder(journal.GetRecords().front(), trade::Status::Accepted));
		data.m_orders.front().m_nQuantity = 200;
		Check(!journal.Reconcile(Account(), MakeStrategyAccountView(data, Security())), "conflicting broker fingerprint disables admission");
		Check(!journal.Prepare(Intent(2, "600001"), MakeStrategyAccountView(data, Security("600001")), 100500).m_bAccepted, "fail closed after journal conflict");
		CStrategyExecution unopened;
		Check(!unopened.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "no journal means no order");
	}

	class CCommitFailureDatabase final : public db::IDataBase
	{
	public:
		explicit CCommitFailureDatabase(db::_TyDBPtr connection) : m_connection(std::move(connection))
		{
		}
		int Connect(const db::CConnectParam& parameters) override
		{
			return m_connection->Connect(parameters);
		}
		int Close() override
		{
			return m_connection->Close();
		}
		int ExecUpdate(const std::string& sql) override
		{
			return m_connection->ExecUpdate(sql);
		}
		int ExecScript(const std::string& sql) override
		{
			return m_connection->ExecScript(sql);
		}
		const db::CTable& ExecQuery(const std::string& sql) override
		{
			return m_connection->ExecQuery(sql);
		}
		bool BeginTransaction() override
		{
			return m_connection->BeginTransaction();
		}
		bool RollBackTransaction() override
		{
			return m_connection->RollBackTransaction();
		}
		void FailCommitResult()
		{
			m_failCommit = true;
		}
		bool EndTransaction() override
		{
			// The server commits, but the caller cannot establish the result.
			bool committed = m_connection->EndTransaction();
			return committed && !m_failCommit;
		}

	private:
		db::_TyDBPtr m_connection;
		bool m_failCommit{ false };
	};

	void TestAmbiguousCommit()
	{
		std::string name = NewTestDatabaseName();
		{
			std::unique_ptr<CCommitFailureDatabase> fault = std::make_unique<CCommitFailureDatabase>(OpenTestDatabase(name));
			CCommitFailureDatabase* borrowed = fault.get();
			db::_TyDBPtr connection(fault.release(), [](db::IDataBase* value)
			{
				delete value;
			});
			CStrategyExecution journal;
			Check(journal.Open(std::move(connection)), "open ambiguous commit store");
			borrowed->FailCommitResult();
			Check(!journal.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "unknown commit cannot admit order");
			Check(!journal.Prepare(Intent(2), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "unknown commit disables further admission");
		}
		CStrategyExecution recovered;
		Check(recovered.Open(OpenTestDatabase(name)), "recover after ambiguous commit");
		Check(1 == recovered.GetRecords().size() && ExecutionState::Unknown == recovered.GetRecords().front().m_state, "committed intent remains unknown after restart");
	}

	void TestSessionOwnership()
	{
		std::string name = NewTestDatabaseName();
		db::_TyDBPtr connection = OpenTestDatabase(name);
		db::IDataBase* borrowed = connection.get();
		CStrategyExecution oldOwner;
		Check(oldOwner.Open(std::move(connection)), "open owning session");
		const db::CTable& released = borrowed->ExecQuery("SELECT RELEASE_ALL_LOCKS()");
		Check(!released.m_rows.empty(), "release test session ownership");
		CStrategyExecution newOwner;
		Check(newOwner.Open(OpenTestDatabase(name)), "new owner acquires released lock");
		Check(!oldOwner.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "old session cannot write after ownership loss");
		Check(newOwner.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "only new owning session can admit");
		db::_TyDBPtr disconnected = OpenTestDatabase();
		db::IDataBase* lostSession = disconnected.get();
		CStrategyExecution lost;
		Check(lost.Open(std::move(disconnected)), "open session before disconnect");
		lostSession->Close();
		Check(0 == lost.AllocateSignal(), "closed session cannot reconnect and allocate IDs");
		Check(!lost.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "disconnected session cannot admit");
	}

	void TestCheckpointRollback()
	{
		std::string name = NewTestDatabaseName();
		db::_TyDBPtr control = OpenTestDatabase(name);
		Check(0 == control->ExecUpdate("CREATE TRIGGER fail_protection BEFORE INSERT ON table_strategy_protection FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected protection failure'"), "install protection storage fault");
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase(name)), "open checkpoint fault store");
		Check(!journal.SaveCheckpoint(42, Account(), Security(), "must-rollback", true), "protection failure rolls back checkpoint");
		const db::CTable& checkpoint = control->ExecQuery("SELECT COUNT(*) FROM table_strategy_checkpoint");
		Check(1 == checkpoint.m_rows.size() && "0" == db::to_string(checkpoint.m_rows.front()[0]), "checkpoint cannot commit without protection lock");
	}

	void TestWriteFailure(const std::string& name)
	{
		{
			CStrategyExecution journal;
			Check(journal.Open(OpenTestDatabase(name)), "create fault-injection journal");
		}
		db::_TyDBPtr database = OpenTestDatabase(name);
		Check(0 == database->ExecUpdate("CREATE TRIGGER fail_execution BEFORE INSERT ON table_strategy_execution FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT='injected storage failure'"), "install MySQL storage fault");
		database.reset();
		CStrategyExecution journal;
		Check(journal.Open(OpenTestDatabase(name)), "open journal with injected storage fault");
		Check(!journal.Prepare(Intent(), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "failed persistence never admits order");
		Check(std::string::npos != journal.GetLastError().find("write failed"), "write failure is not misreported as a duplicate");
		Check(!journal.Prepare(Intent(2), MakeStrategyAccountView(Data(), Security()), 100500).m_bAccepted, "storage failure disables subsequent admission");
		Check(journal.GetRecords().empty(), "rolled back insertion has no prepared intent");
	}
} // namespace

int main()
{
	if (!HasTestMySql())
	{
		std::cout << "Set XENGINE_TEST_MYSQL_HOST for isolated MySQL integration tests\n";
		return 77;
	}
	CTestDatabases databases;
	try
	{
		std::string name = NewTestDatabaseName();
		TestSnapshot();
		TestAdmission();
		TestWatermarks();
		TestHandoff();
		TestConcurrentAdmission();
		TestSellWatermarks();
		TestMissingCoveredOrder();
		TestDuplicateBrokerIdentity();
		TestRecovery(name);
		TestProtection();
		TestCheckpointOwnership();
		TestConflict();
		TestWriteFailure(NewTestDatabaseName());
		TestAmbiguousCommit();
		TestSessionOwnership();
		TestCheckpointRollback();
		std::cout << "Strategy execution tests passed: snapshots, reservations, resource fences, concurrency, durable recovery, locks and conflicts\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
