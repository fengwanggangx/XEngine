#include "../strategies/CDualMaCrossStrategy.h"
#include "../strategies/CTakeProfitStopLossStrategy.h"
#include "../strategies/CTrailingTakeProfitStrategy.h"
#include "../strategies/CPriceBreakoutStrategy.h"
#include "../IStrategyContext.h"
#include "../../indicator/CIndicator.h"
#include "MySqlTestDatabase.h"

#include <iostream>
#include <stdexcept>

namespace
{
	void Check(bool value, const char* message)
	{
		if (!value)
		{
			throw std::runtime_error(message);
		}
	}

	CSecurity Security()
	{
		return CSecurity("600000", Exchange::sse);
	}

	_TyAccount Account()
	{
		return { 1, "000123" };
	}

	class CContext final : public IStrategyContext
	{
	public:
		CContext()
		{
			Check(m_execution.Open(OpenTestDatabase()), "open execution journal");
			m_data.m_ready = true;
			m_data.m_version = 10;
			m_data.m_orders_version = 10;
			m_data.m_positions_version = 10;
			m_data.m_funds_version = 10;
			m_data.m_account.m_account = Account();
			m_data.m_account.m_connected = true;
			m_data.m_account.m_balance = 10000000;
			m_data.m_account.m_available = 10000000;
			m_data.m_account.m_explicit_available = true;
			trade::CPosition position;
			position.m_account = Account();
			position.m_security = Security().String();
			position.m_total = 300;
			position.m_available = 100;
			position.m_cost = 300000;
			m_data.m_positions.emplace_back(position);
			m_rules.m_reliable_query = true;
			m_rules.m_idempotent_client_id = true;
			m_rules.m_calendar = { 20261118, 900000, 1500000, 1600000, 2000000 };
			m_rules.m_rule.m_version_text = "test-rules";
			m_rules.m_rule.m_security = Security().String();
			m_rules.m_rule.m_lower = 1;
			m_rules.m_rule.m_upper = 100000;
		}

		COrderSubmitResult SubmitOrder(const COrderIntent& intent) override
		{
			COrderIntent routed = intent;
			routed.m_account = Account();
			COrderSubmitResult result = m_execution.Prepare(routed, GetTradingSnapshot(Security()), intent.m_maximum_debit);
			if (result.m_accepted)
			{
				routed.m_client_order_id = result.m_client_order_id;
				m_orders.emplace_back(routed);
				m_execution.MarkSubmission(result.m_client_order_id, true);
			}
			return result;
		}

		bool CancelOrder(_TyOrderId) override
		{
			return true;
		}

		CPositionSnapshot GetPosition(const CSecurity& security) const override
		{
			return GetTradingSnapshot(security).m_position;
		}

		CAccountSnapshot GetAccount() const override
		{
			return GetTradingSnapshot(Security()).m_account;
		}

		void WriteLog(LogLevel, const std::string& message) override
		{
			m_reason = message;
		}

		bool IsMarketAvailable() const override
		{
			return true;
		}

		CStrategyAccountView GetTradingSnapshot(const CSecurity& security) const override
		{
			return MakeStrategyAccountView(m_data, security);
		}

		std::optional<trade::CExecutionRule> GetRules(const CSecurity&) override
		{
			return m_rules_available ? std::optional<trade::CExecutionRule>(m_rules) : std::nullopt;
		}

		std::string LoadCheckpoint() const override
		{
			return m_execution.LoadCheckpoint(42);
		}

		bool SaveCheckpoint(const std::string& data, bool protect) override
		{
			return m_execution.SaveCheckpoint(42, Account(), Security(), data, protect);
		}

		std::uint64_t AllocateSignal() override
		{
			return m_execution.AllocateSignal();
		}

		std::optional<CExecutionRecord> FindExecution(std::uint64_t id) const override
		{
			return m_execution.FindExecution(id);
		}

		std::optional<CExecutionRecord> FindSignal(std::uint64_t id) const override
		{
			return m_execution.FindSignal(42, id);
		}

		bool ClaimExit() override
		{
			return m_execution.ClaimExit(Account(), Security(), 42);
		}

		bool ReleaseExit(bool unlock) override
		{
			return m_execution.ReleaseExit(Account(), Security(), 42) && (!unlock || m_execution.UnlockBuys(Account(), Security(), 42));
		}

		bool CancelConflicts(bool force = false) override
		{
			if (force)
			{
				m_checkpointAtCancel = LoadCheckpoint();
			}
			return true;
		}

		bool RequestHistory(std::int64_t) override
		{
			return true;
		}

		std::int64_t AvailableFunds(const CStrategyAccountView& view) const override
		{
			return m_execution.AvailableFunds(Account(), view);
		}

		void PublishBusiness(const std::string& state, const std::string& details) override
		{
			m_phase = state;
			m_reason = details;
		}

		std::int64_t NowMs() const override
		{
			return m_now;
		}

		void Quote(IStrategy& strategy, std::int64_t price)
		{
			_TyQuoteData quote;
			quote.mutable_security()->set_symbol("600000");
			quote.mutable_security()->set_exchange(hqmarket::market::v1::SSE);
			quote.set_price_scale(2);
			quote.set_last_price(price);
			quote.set_receive_time_ms(++m_now);
			strategy.OnQuote(quote);
		}

		void Fill(IStrategy& strategy, std::int64_t remaining, std::int64_t available)
		{
			const COrderIntent& intent = m_orders.back();
			trade::COrder order;
			order.m_account = Account();
			order.m_client_id = intent.m_client_order_id;
			order.m_order_id = order.m_client_id;
			order.m_id = 42;
			order.m_signal_id = intent.m_signal_id;
			order.m_security = Security().String();
			order.m_side = OrderSide::Buy == intent.m_side ? trade::Side::Buy : trade::Side::Sell;
			order.m_price = intent.m_price;
			order.m_price_scale = intent.m_price_scale;
			order.m_quantity = intent.m_quantity;
			order.m_filled = intent.m_quantity;
			order.m_status = trade::Status::Filled;
			m_data.m_orders.emplace_back(order);
			m_data.m_positions.front().m_total = remaining;
			m_data.m_positions.front().m_available = available;
			m_data.m_version += 10;
			m_data.m_orders_version = m_data.m_funds_version = m_data.m_positions_version = m_data.m_version;
			Check(m_execution.Reconcile(Account(), GetTradingSnapshot(Security())), "reconcile completed order");
			strategy.OnOrder({});
		}

		mutable CStrategyExecutionManager m_execution;
		trade::CAccountData m_data;
		trade::CExecutionRule m_rules;
		bool m_rules_available{ true };
		std::int64_t m_now{ 1000000 };
		std::vector<COrderIntent> m_orders;
		std::string m_checkpointAtCancel;
		std::string m_phase;
		std::string m_reason;
	};

	CStrategyConfig Config(const std::string& type, strategy::_TyParameters parameters)
	{
		CStrategyConfig config;
		config.m_id = 42;
		config.m_account = Account();
		config.m_name = type;
		config.m_type = type;
		config.m_params = std::move(parameters);
		config.m_subscriptions.emplace_back("600000", Exchange::sse, Channel::quote);
		if ("dual_ma_cross" == type)
		{
			config.m_subscriptions.emplace_back("600000", Exchange::sse, Channel::bar_1m);
		}
		return config;
	}

	_TyBarData Bar(int index, std::int64_t price, bool completed = true)
	{
		_TyBarData bar;
		bar.mutable_security()->set_symbol("600000");
		bar.mutable_security()->set_exchange(hqmarket::market::v1::SSE);
		bar.set_channel(hqmarket::market::v1::CHANNEL_BAR_1M);
		bar.set_basis_version("fixture-basis");
		bar.set_begin_time_ms(index * 60000);
		bar.set_end_time_ms((index + 1) * 60000);
		bar.set_completed(completed);
		bar.set_adjustment("none");
		bar.set_close_price(price);
		bar.set_price_scale(2);
		bar.set_data_version(1);
		return bar;
	}

	void TestParameters()
	{
		strategy::CSettings settings;
		std::string error;
		Check(!strategy::CSettings::Read("price_breakout", { { "trigger_price", "1.-2" } }, settings, error), "negative fraction is invalid");
		Check(!strategy::CSettings::Read("take_profit_stop_loss", {}, settings, error), "missing protective threshold rejected");
		Check(!strategy::CSettings::Read("dual_ma_cross", { { "short_window", "20" }, { "long_window", "5" } }, settings, error), "window ordering");
		Check(!strategy::CSettings::Read("trailing_take_profit", { { "activation_value", "5" }, { "drawdown_value", "100" } }, settings, error), "drawdown must be less than 100 percent");
	}

	void TestBreakout()
	{
		CContext context;
		CPriceBreakoutStrategy strategy;
		Check(strategy.Initialize(context, Config("price_breakout", { { "trigger_price", "10" }, { "auto_rearm", "1" } })), "initialize breakout");
		strategy.OnStart();
		context.Quote(strategy, 1000);
		Check(context.m_orders.empty(), "first quote establishes baseline");
		context.Quote(strategy, 900);
		context.Quote(strategy, 1000);
		Check(1 == context.m_orders.size(), "upward threshold crossing");
		context.Quote(strategy, 1100);
		Check(1 == context.m_orders.size(), "pending order suppresses duplicate crossing");
		context.Fill(strategy, 400, 100);
		context.Quote(strategy, 1100);
		Check(1 == context.m_orders.size(), "no repeated buy while condition stays true");
		context.Quote(strategy, 900);
		context.Quote(strategy, 1000);
		Check(2 == context.m_orders.size(), "recross re-arms after full fill");
	}

	void TestProtection()
	{
		CContext context;
		CTakeProfitStopLossStrategy strategy;
		Check(strategy.Initialize(context, Config("take_profit_stop_loss", { { "stop_loss_value", "5" }, { "auto_rearm", "1" } })), "initialize stop loss");
		strategy.OnStart();
		context.Quote(strategy, 950);
		Check(1 == context.m_orders.size() && 100 == context.m_orders.back().m_quantity, "first quote can trigger, only sell available shares");
		Check(context.m_execution.IsBuyLocked(Account(), Security()), "protective trigger locks buys");
		context.Fill(strategy, 200, 0);
		context.Quote(strategy, 940);
		Check(1 == context.m_orders.size(), "unavailable residual remains protected");
		Check(strategy.OnAction("pause_execution"), "pause remaining exit");
		std::string paused = context.LoadCheckpoint();
		Check(strategy.OnAction("resume_execution"), "resume remaining exit");
		Check(!context.m_checkpointAtCancel.empty() && paused != context.m_checkpointAtCancel, "resume checkpoint is durable before canceling conflicts");
		CTakeProfitStopLossStrategy recovered;
		Check(recovered.Initialize(context, Config("take_profit_stop_loss", { { "stop_loss_value", "5" }, { "auto_rearm", "1" } })), "restore protected task checkpoint");
		recovered.OnStart();
		context.Quote(recovered, 940);
		Check(1 == context.m_orders.size(), "restored task does not repeat filled portion");
		context.m_data.m_positions.front().m_available = 200;
		context.m_data.m_version += 1;
		context.Quote(recovered, 930);
		Check(2 == context.m_orders.size() && 200 == context.m_orders.back().m_quantity, "continue original task when shares become available");
		context.Fill(recovered, 0, 0);
		context.Quote(recovered, 930);
		Check(!context.m_execution.IsBuyLocked(Account(), Security()), "auto-rearm releases own lock after proven flat position");
	}

	void TestTrailing()
	{
		CContext context;
		CTrailingTakeProfitStrategy strategy;
		Check(strategy.Initialize(context, Config("trailing_take_profit", { { "activation_value", "5" }, { "drawdown_value", "2" } })), "initialize trailing profit");
		strategy.OnStart();
		context.Quote(strategy, 1040);
		context.Quote(strategy, 1050);
		context.Quote(strategy, 1200);
		context.Quote(strategy, 1177);
		Check(context.m_orders.empty(), "track without premature drawdown");
		context.Quote(strategy, 1176);
		Check(1 == context.m_orders.size(), "exact 2 percent retracement triggers");
	}

	void TestMovingAverage(CIndicator& indicator)
	{
		for (const std::string& algorithm : { std::string("sma"), std::string("ema") })
		{
			CContext context;
			CDualMaCrossStrategy strategy(indicator);
			Check(strategy.Initialize(context, Config("dual_ma_cross", { { "short_window", "2" }, { "long_window", "3" }, { "ma_type", algorithm } })), "initialize bar MA");
			strategy.OnStart();
			context.Quote(strategy, 400);
			strategy.OnHistory({ Bar(1, 300), Bar(2, 200), Bar(3, 100) });
			Check(context.m_orders.empty(), "history prewarm does not trade");
			strategy.OnBar(Bar(4, 1000, false));
			Check(context.m_orders.empty(), "unfinished bar ignored");
			strategy.OnBar(Bar(4, 1000));
			Check(1 == context.m_orders.size() && OrderSide::Buy == context.m_orders.front().m_side, "completed bar cross buys for SMA and EMA");
			strategy.OnBar(Bar(4, 1000));
			Check(1 == context.m_orders.size(), "duplicate bar ignored");
			_TyBarData changed = Bar(5, 1);
			changed.set_basis_version("changed-basis");
			strategy.OnBar(changed);
			Check(1 == context.m_orders.size() && "待处理" == context.m_phase, "basis change pauses signals");
		}
	}

	void TestUnavailableAndUnknown()
	{
		CContext context;
		CPriceBreakoutStrategy strategy;
		Check(strategy.Initialize(context, Config("price_breakout", { { "trigger_price", "10" } })), "initialize gated strategy");
		strategy.OnStart();
		context.m_rules_available = false;
		context.Quote(strategy, 900);
		context.Quote(strategy, 1100);
		Check(context.m_orders.empty(), "no guessed execution rules");
		context.m_rules_available = true;
		context.Quote(strategy, 900);
		context.Quote(strategy, 1000);
		Check(1 == context.m_orders.size(), "rules enable execution");
		context.m_execution.MarkSubmission(context.m_orders.back().m_client_order_id, false);
		context.Quote(strategy, 1100);
		Check(!strategy.OnAction("rearm") && !strategy.OnAction("resume_execution"), "unknown result cannot be reset or resent");
	}
} // namespace

int main()
{
	if (!HasTestMySql())
	{
		std::cout << "Set XENGINE_TEST_MYSQL_HOST for isolated MySQL strategy tests\n";
		return 77;
	}
	CTestDatabases databases;
	try
	{
		CIndicator indicator;
		std::string error;
		Check(indicator.Initialize(error), "indicator runtime");
		TestParameters();
		TestBreakout();
		TestProtection();
		TestTrailing();
		TestMovingAverage(indicator);
		TestUnavailableAndUnknown();
		std::cout << "Four-strategy behavior tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
