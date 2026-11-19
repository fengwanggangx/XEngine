#include "datetime.h"
#include <chrono>
#include <unordered_map>
#include <format>

namespace datetime
{
	std::string_view GetDateTimeFmt(datetime::precision pr)
	{
		static std::unordered_map<precision, std::string> s_format{
			{ datetime::precision::year, "{:%Y}" },
			{ datetime::precision::year_day, "{:%Y%m%d}" },
			{ datetime::precision::year_seconds, "{:%Y%m%d%H%M%S}" },
			{ datetime::precision::month, "{:%m}" },
			{ datetime::precision::month_day, "{:%m%d}" },
			{ datetime::precision::month_seconds, "{:%m%d%H%M%S}" },
			{ datetime::precision::day_seconds, "{:%d%H%M%S}" },
			{ datetime::precision::day, "{:%d}" },
		};
		const auto mIter = s_format.find(pr);
		if (mIter == s_format.end())
		{
			return "";
		}
		return mIter->second;
	}

	std::string GetSysDateTime(datetime::precision pr)
	{
		std::string_view strFmt = GetDateTimeFmt(pr);
		if (strFmt.empty())
		{
			return "";
		}
		std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
		auto now_seconds = std::chrono::floor<std::chrono::seconds>(now);
		return std::vformat(strFmt, std::make_format_args(now_seconds));
	}
} // namespace datetime
