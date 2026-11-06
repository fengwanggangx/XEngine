#include "../CIndicator.h"

#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
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
} // namespace

int main()
{
	try
	{
		CIndicator indicator;
		std::string error;
		std::array<double, 5> prices{ 1, 2, 3, 4, 5 };
		double value = -1;
		Check(!indicator.CalculateSma(prices, 3, value, error) && (-1 == value), "uninitialized calculation preserves output");
		Check(indicator.Initialize(error), "initialize");
		Check(indicator.Initialize(error), "same owner initializes idempotently");
		Check(indicator.CalculateSma(prices, 3, value, error) && (4 == value), "latest SMA uses the tail, not the first output");
		Check(indicator.CalculateSma(prices, 1, value, error) && (5 == value), "period one compatibility");
		for (std::size_t period : { std::size_t(0), std::size_t(6), std::size_t(100001) })
		{
			value = -1;
			Check(!indicator.CalculateSma(prices, period, value, error) && !error.empty() && (-1 == value), "invalid period or warmup preserves output");
		}
		Check(!indicator.CalculateSma({}, 1, value, error), "empty input");
		for (double invalid : { std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
		{
			prices[0] = invalid;
			Check(!indicator.CalculateSma(prices, 3, value, error) && (-1 == value), "nonfinite input preserves output");
		}
		prices[0] = 1;
		std::vector<double> maximumWindow(100000, 7.0);
		Check(indicator.CalculateSma(maximumWindow, maximumWindow.size(), value, error) && (7 == value), "maximum SMA window");
		CIndicator other;
		Check(!other.Initialize(error), "second owner cannot reset process-wide state");
		other.Finalize();
		Check(indicator.CalculateSma(prices, 3, value, error), "failed owner cannot shut down active owner");
		std::atomic_bool correct{ true };
		std::vector<std::jthread> workers;
		for (int index = 0; 8 > index; ++index)
		{
			workers.emplace_back([&indicator, &prices, &correct]()
			{
				for (int iteration = 0; 1000 > iteration; ++iteration)
				{
					std::string threadError;
					double threadValue = 0;
					if (!indicator.CalculateSma(prices, 3, threadValue, threadError) || (4 != threadValue))
					{
						correct.store(false);
					}
				}
			});
		}
		workers.clear();
		Check(correct.load(), "independent concurrent calculations");
		indicator.Finalize();
		indicator.Finalize();
		value = -1;
		Check(!indicator.CalculateSma(prices, 1, value, error) && (-1 == value), "shutdown disables even period one");
		Check(other.Initialize(error), "new owner can initialize after shutdown");
		other.Finalize();
		Check(indicator.Initialize(error), "restart after shutdown");
		std::cout << "Indicator tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
