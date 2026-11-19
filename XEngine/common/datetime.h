#ifndef __DATETIME_H__
#define __DATETIME_H__
#include <string>

namespace datetime
{
	enum class precision
	{
		year = 0,
		year_day,
		year_seconds,
		month,
		month_day,
		month_seconds,
		day_seconds,
		day
	};

	std::string GetSysDateTime(datetime::precision pr);
} // namespace datetime

#endif
