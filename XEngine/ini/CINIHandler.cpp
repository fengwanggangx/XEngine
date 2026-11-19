#include "CINIHandler.h"

#include <array>
#include <filesystem>
#include <utility>

namespace ini
{
	namespace
	{
		using _TyCfg = std::pair<Config, std::filesystem::path>;
		const std::array<_TyCfg, 1> configFiles{
			_TyCfg{ Config::System, std::filesystem::path("ini") / "system.ini" }
		};
	} // namespace

	CINIHandler::CINIHandler()
	{
		Load();
	}

	CINIHandler::~CINIHandler() = default;

	std::vector<std::pair<std::string, std::string>> CINIHandler::GetSection(Config config, const std::string& strSection) const
	{
		auto mIter = m_ini_files.find(config);
		if (m_ini_files.end() == mIter)
		{
			return {};
		}
		return mIter->second->GetSection(strSection);
	}

	bool CINIHandler::UpdateEntry(Config config, const std::string& strSection, const std::string& strKey, const std::optional<std::string>& value, const std::string& oldKey)
	{
		auto mIter = m_ini_files.find(config);
		return m_ini_files.end() != mIter && mIter->second->UpdateEntry(strSection, strKey, value, oldKey);
	}

	bool CINIHandler::Load()
	{
		if (!m_ini_files.empty())
		{
			return true;
		}

		for (const auto& v : configFiles)
		{
			m_ini_files.emplace(v.first, std::make_unique<CIniFile>(v.second.string()));
		}
		return true;
	}
} // namespace ini
