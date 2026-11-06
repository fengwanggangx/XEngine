#include "../strategies/CMovingAverageStrategy.h"
#include "../IStrategyContext.h"
#include "../../indicator/CIndicator.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
	void Check(bool passed, const char* message)
	{
		if (!passed)
		{
			throw std::runtime_error(message);
		}
	}

	class CContext final : public IStrategyContext
	{
	public:
		COrderSubmitResult SubmitOrder(const COrderIntent& intent) override
		{
			m_orders.emplace_back(intent);
			return { true, false, intent.m_signalId, 0, {} };
		}
		bool CancelOrder(_TyOrderId) override
		{
			return false;
		}
		CPositionSnapshot GetPosition(const CSecurity&) const override
		{
			return {};
		}
		CAccountSnapshot GetAccount() const override
		{
			return {};
		}
		void WriteLog(LogLevel, const std::string& message) override
		{
			m_logs.emplace_back(message);
		}
		bool IsMarketAvailable() const override
		{
			return true;
		}

		std::vector<COrderIntent> m_orders;
		std::vector<std::string> m_logs;
	};

	CStrategyConfig Configuration()
	{
		CStrategyConfig config;
		config.m_id = 42;
		config.m_params = { { "short_window", "2" }, { "long_window", "3" } };
		config.m_subscriptions = { CQuoteInfo("600000", Exchange::sse, Channel::quote), CQuoteInfo("600000", Exchange::szse, Channel::quote) };
		return config;
	}

	void Feed(CMovingAverageStrategy& strategy, std::int64_t price, int exchange = 1, int scale = 2)
	{
		_TyQuoteData quote;
		quote.mutable_security()->set_symbol("600000");
		quote.mutable_security()->set_exchange(static_cast<hqmarket::market::v1::Exchange>(exchange));
		quote.set_last_price(price);
		quote.set_price_scale(scale);
		strategy.OnQuote(quote);
	}

	void TestLegacySignals(CIndicator& indicator)
	{
		CContext context;
		CMovingAverageStrategy strategy(indicator);
		Check(strategy.Initialize(context, Configuration()), "initialize strategy");
		strategy.OnStart();
		std::vector<std::int64_t> prices{ 500, 400, 300, 600, 100, 100, 700, 800, 200, 100 };
		std::vector<OrderSide> expected;
		int previousRelation = 0;
		for (std::size_t index = 0; prices.size() > index; ++index)
		{
			Feed(strategy, prices[index]);
			if (2 > index)
			{
				Check(context.m_orders.empty(), "warmup emits no orders");
				continue;
			}
			std::int64_t shortSum = prices[index - 1] + prices[index];
			std::int64_t longSum = prices[index - 2] + shortSum;
			int relation = shortSum * 3 > longSum * 2 ? 1 : (shortSum * 3 < longSum * 2 ? -1 : 0);
			if ((0 != relation) && (0 != previousRelation) && (relation != previousRelation))
			{
				expected.emplace_back(0 < relation ? OrderSide::Buy : OrderSide::Sell);
			}
			previousRelation = relation;
			Check(expected.size() == context.m_orders.size(), "normal sequence matches old integer strategy");
			for (std::size_t orderIndex = 0; expected.size() > orderIndex; ++orderIndex)
			{
				Check(expected[orderIndex] == context.m_orders[orderIndex].m_side, "legacy side matches");
			}
		}
		Check(3 <= context.m_orders.size(), "sequence exercises repeated crossings");
		for (std::size_t index = 0; context.m_orders.size() > index; ++index)
		{
			const COrderIntent& order = context.m_orders[index];
			Check((42 == order.m_id) && (index + 1 == order.m_signalId) && (100 == order.m_quantity), "strategy routing and quantity unchanged");
			Check((OrderType::Limit == order.m_orderType) && (2 == order.m_nPriceScale) && (Exchange::sse == order.m_security.m_market), "limit order and original security preserved");
			Check((OrderSide::Buy == order.m_side ? PositionEffect::Open : PositionEffect::Close) == order.m_positionEffect, "position effect unchanged");
		}
		Check(600 == context.m_orders.front().m_price, "order uses original fixed-point price");
	}

	void TestMultipleSecurities(CIndicator& indicator)
	{
		CContext mixedContext;
		CContext firstContext;
		CContext secondContext;
		CMovingAverageStrategy mixed(indicator);
		CMovingAverageStrategy first(indicator);
		CMovingAverageStrategy second(indicator);
		Check(mixed.Initialize(mixedContext, Configuration()) && first.Initialize(firstContext, Configuration()) && second.Initialize(secondContext, Configuration()), "initialize independent strategies");
		mixed.OnStart();
		first.OnStart();
		second.OnStart();
		std::vector<std::int64_t> firstPrices{ 500, 400, 300, 600, 100, 100, 700 };
		std::vector<std::int64_t> secondPrices{ 10000, 11000, 12000, 11500, 10000, 9000, 10500 };
		for (std::size_t index = 0; firstPrices.size() > index; ++index)
		{
			Feed(mixed, firstPrices[index], 1);
			Feed(mixed, secondPrices[index], 2);
			Feed(first, firstPrices[index], 1);
			Feed(second, secondPrices[index], 2);
		}
		for (int exchange : { 1, 2 })
		{
			std::vector<OrderSide> actual;
			for (const auto& order : mixedContext.m_orders)
			{
				if (exchange == static_cast<int>(order.m_security.m_market))
				{
					actual.emplace_back(order.m_side);
				}
			}
			const std::vector<COrderIntent>& expected = 1 == exchange ? firstContext.m_orders : secondContext.m_orders;
			Check(actual.size() == expected.size(), "exchanges with the same symbol have independent windows");
			for (std::size_t index = 0; actual.size() > index; ++index)
			{
				Check(actual[index] == expected[index].m_side, "interleaving does not change signal direction");
			}
		}
		for (std::size_t index = 0; mixedContext.m_orders.size() > index; ++index)
		{
			Check(index + 1 == mixedContext.m_orders[index].m_signalId, "all securities share the instance signal sequence");
		}
	}

	void TestLifecycleAndPrecision(CIndicator& indicator)
	{
		CContext context;
		CMovingAverageStrategy strategy(indicator);
		Check(strategy.Initialize(context, Configuration()), "initialize lifecycle strategy");
		strategy.OnStart();
		Feed(strategy, 500, 1, 2);
		Feed(strategy, 4000, 1, 3);
		Feed(strategy, 30000, 1, 4);
		Check(context.m_orders.empty(), "first relation only establishes state");
		strategy.OnPause();
		Feed(strategy, 9900);
		strategy.OnStart();
		Feed(strategy, 60000, 1, 4);
		Check((1 == context.m_orders.size()) && (60000 == context.m_orders.back().m_price) && (4 == context.m_orders.back().m_nPriceScale), "pause preserves warmup and mixed scales normalize consistently");
		strategy.OnStop();
		strategy.OnStart();
		Feed(strategy, 500);
		Feed(strategy, 400);
		Feed(strategy, -1);
		Feed(strategy, 1, 1, -1);
		Feed(strategy, 1, 1, 19);
		Check(1 == context.m_orders.size(), "stop clears warmup and invalid quotes do not advance it");
		Feed(strategy, 300);
		Check(1 == context.m_orders.size(), "restart first relation emits no order");
		Feed(strategy, 600);
		Check((2 == context.m_orders.size()) && (2 == context.m_orders.back().m_signalId), "restart preserves signal sequence");
		indicator.Finalize();
		Feed(strategy, 500);
		Feed(strategy, 400);
		Check(1 == context.m_logs.size(), "continuous calculation failure is logged once");
		std::string error;
		Check(indicator.Initialize(error), "recover indicator");
		Feed(strategy, 300);
		indicator.Finalize();
		Feed(strategy, 200);
		Check(2 == context.m_logs.size(), "recovery permits a later failure log");
		Check(indicator.Initialize(error), "restore runtime for remaining tests");
		CStrategyConfig invalid = Configuration();
		invalid.m_params["long_window"] = "100001";
		Check(!CMovingAverageStrategy::ValidateParameters(invalid), "reject invalid disabled configuration before persistence");
		Check(!strategy.Initialize(context, invalid), "reject unsupported window before processing quotes");
		invalid = Configuration();
		invalid.m_params["short_window"] = "1";
		Check(strategy.Initialize(context, invalid), "preserve period one");
	}

	void TestEqualBoundary(CIndicator& indicator)
	{
		CContext context;
		CMovingAverageStrategy strategy(indicator);
		CStrategyConfig config = Configuration();
		config.m_params["short_window"] = "1";
		config.m_params["long_window"] = "2";
		Check(strategy.Initialize(context, config), "initialize tolerance strategy");
		strategy.OnStart();
		Feed(strategy, 10000000000000LL, 1, 4);
		Feed(strategy, 10000000000001LL, 1, 4);
		Feed(strategy, 10000000000000LL, 1, 4);
		Feed(strategy, 10000000000001LL, 1, 4);
		Check(context.m_orders.empty(), "sub-tolerance changes cannot manufacture crossings");
	}
} // namespace

int main()
{
	try
	{
		CIndicator indicator;
		std::string error;
		Check(indicator.Initialize(error), "initialize indicator");
		TestLegacySignals(indicator);
		TestMultipleSecurities(indicator);
		TestLifecycleAndPrecision(indicator);
		TestEqualBoundary(indicator);
		std::cout << "Moving-average tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
