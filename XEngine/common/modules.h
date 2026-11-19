#ifndef __MODULES_H__
#define __MODULES_H__

#include <string>

namespace md
{
	enum class modules
	{
		global = 0,
		database,
		settings
	};

	std::string GetModuleName(md::modules m);
} // namespace md

#endif
