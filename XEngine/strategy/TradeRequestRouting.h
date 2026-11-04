#pragma once
#include "../request/request.h"
#include <charconv>

namespace tradingroute
{
	inline bool ReadNumber(const CRequest& request, const std::string& key, std::uint64_t& value)
	{
		std::string text = request.GetExtraData(key);
		std::from_chars_result result = std::from_chars(text.data(), text.data() + text.size(), value);
		return std::errc{} == result.ec && text.data() + text.size() == result.ptr;
	}
} // namespace tradingroute
