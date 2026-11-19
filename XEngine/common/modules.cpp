#include "modules.h"

#include <unordered_map>

namespace md
{
	std::string GetModuleName(md::modules m)
	{
		static std::unordered_map<md::modules, std::string> s_name{
			{ md::modules::global, "global" },
			{ md::modules::database, "database" }
		};
		const auto mIter = s_name.find(m);
		if (mIter == s_name.end())
		{
			return "";
		}
		return mIter->second;
	}
} // namespace md
