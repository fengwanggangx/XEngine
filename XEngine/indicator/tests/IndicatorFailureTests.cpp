#include "../CIndicator.h"
#include <ta_libc.h>

#include <array>
#include <iostream>

namespace
{
	int initializeCalls = 0;
	int shutdownCalls = 0;
} // namespace

// Link this executable without upstream sources to exercise startup failure.
extern "C" TA_RetCode TA_Initialize(void)
{
	++initializeCalls;
	return TA_ALLOC_ERR;
}

extern "C" TA_RetCode TA_Shutdown(void)
{
	++shutdownCalls;
	return TA_SUCCESS;
}

extern "C" TA_RetCode TA_SMA(int, int, const double[], int, int*, int*, double[])
{
	return TA_INTERNAL_ERROR;
}

int main()
{
	{
		CIndicator indicator;
		std::string error;
		double value = 123;
		std::array<double, 2> prices{ 1, 2 };
		if (indicator.Initialize(error) || error.empty() || indicator.CalculateSma(prices, 2, value, error) || (123 != value))
		{
			return 1;
		}
		indicator.Finalize();
		indicator.Finalize();
		if (indicator.Initialize(error))
		{
			return 1;
		}
	}
	if ((2 != initializeCalls) || (0 != shutdownCalls))
	{
		return 1;
	}
	std::cout << "Indicator initialization failure tests passed\n";
	return 0;
}
