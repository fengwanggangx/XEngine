#pragma once

#include <cstdint>
#include <limits>

namespace utility
{
	inline bool MultiplyDivide(std::int64_t value, std::int64_t factor, std::int64_t divisor, std::int64_t& result, bool roundUp = false)
	{
		if ((0 > value) || (0 > factor) || (0 >= divisor) || ((0 != factor) && (value > (std::numeric_limits<std::int64_t>::max)() / factor)))
		{
			return false;
		}
		std::int64_t product = value * factor;
		result = product / divisor + ((roundUp && (0 != product % divisor)) ? 1 : 0);
		return true;
	}

} // namespace utility
