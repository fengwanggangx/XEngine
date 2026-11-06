#include "CIndicator.h"

#include <ta_libc.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <shared_mutex>

namespace
{
	// TA-Lib has process-wide settings. One owner controls their entire lifetime.
	std::shared_mutex runtimeMutex;
	CIndicator* runtimeOwner{ nullptr };
} // namespace

CIndicator::~CIndicator()
{
	Finalize();
}

bool CIndicator::Initialize(std::string& error)
{
	error.clear();
	std::unique_lock<std::shared_mutex> lock(runtimeMutex);
	if (this == runtimeOwner)
	{
		return true;
	}
	if (nullptr != runtimeOwner)
	{
		error = "Indicator runtime already has an owner";
		return false;
	}
	TA_RetCode code = TA_Initialize();
	if (TA_SUCCESS != code)
	{
		error = "TA_Initialize failed: " + std::to_string(static_cast<int>(code));
		return false;
	}
	runtimeOwner = this;
	return true;
}

void CIndicator::Finalize()
{
	std::unique_lock<std::shared_mutex> lock(runtimeMutex);
	if (this == runtimeOwner)
	{
		TA_Shutdown();
		runtimeOwner = nullptr;
	}
}

bool CIndicator::CalculateSma(std::span<const double> prices, std::size_t period, double& result, std::string& error) const
{
	error.clear();
	std::shared_lock<std::shared_mutex> lock(runtimeMutex);
	if (this != runtimeOwner)
	{
		error = "Indicator runtime is not initialized";
		return false;
	}
	if ((0 == period) || (100000 < period) || (prices.size() > static_cast<std::size_t>(TA_INDEX_MAX) + 1))
	{
		error = "SMA period must be between 1 and 100000 and input index must not exceed TA_INDEX_MAX";
		return false;
	}
	if (prices.size() < period)
	{
		error = "Insufficient SMA samples";
		return false;
	}
	if (!std::all_of(prices.begin(), prices.end(), [](double price)
	{
		return std::isfinite(price);
	}))
	{
		error = "SMA samples must be finite";
		return false;
	}
	double value = prices.back();
	if (1 != period)
	{
		int beginIndex = 0;
		int outputCount = 0;
		int lastIndex = static_cast<int>(prices.size()) - 1;
		TA_RetCode code = TA_SMA(lastIndex, lastIndex, prices.data(), static_cast<int>(period), &beginIndex, &outputCount, &value);
		if ((TA_SUCCESS != code) || (lastIndex != beginIndex) || (1 != outputCount) || !std::isfinite(value))
		{
			error = "TA_SMA failed or returned invalid output: " + std::to_string(static_cast<int>(code));
			return false;
		}
	}
	result = value;
	return true;
}
