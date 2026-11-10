#ifndef HQMARKET_INI_CINIHANDLER_H
#define HQMARKET_INI_CINIHANDLER_H

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../common/ISingleton.h"
#include "CIniFile.h"

namespace ini
{
	enum class Config
	{
		System = 0,
		Ui,
		Sqlite,
		Mysql,
		Oracle
	};

	class CINIHandler : public ISingleton<CINIHandler>
	{
		DECLARE_SINGLE_DFAULT(CINIHandler)

	  public:
		int GetInt(Config config, const std::string& strSection, const std::string& strKey) const
		{
			return GetValue(config, strSection, strKey, -1);
		}

		std::string GetString(Config config, const std::string& strSection, const std::string& strKey) const
		{
			return GetValue(config, strSection, strKey, std::string());
		}

		template <class Type, std::enable_if_t<!std::is_convertible<Type, std::string>::value, int> = 0>
		Type GetValue(Config config, const std::string& strSection, const std::string& strKey, Type&& defaultValue) const
		{
			const auto& mIter = m_iniFiles.find(config);
			if (m_iniFiles.end() == mIter)
			{
				return std::forward<Type>(defaultValue);
			}
			return mIter->second->GetValue(strSection, strKey, std::forward<Type>(defaultValue));
		}

		template <class Type, std::enable_if_t<std::is_convertible<Type, std::string>::value, int> = 0>
		std::string GetValue(Config config, const std::string& strSection, const std::string& strKey, Type&& defaultValue) const
		{
			const auto& mIter = m_iniFiles.find(config);
			if (m_iniFiles.end() == mIter)
			{
				return std::forward<Type>(defaultValue);
			}
			return mIter->second->GetString(strSection, strKey, std::forward<Type>(defaultValue));
		}

		template <class Type>
		bool SetValue(Config config, const std::string& strSection, const std::string& strKey, Type&& value)
		{
			const auto& mIter = m_iniFiles.find(config);
			if (m_iniFiles.end() == mIter)
			{
				return false;
			}
			return mIter->second->SetValue(strSection, strKey, std::forward<Type>(value));
		}

		bool Load();
		std::vector<std::pair<std::string, std::string>> GetSection(Config config, const std::string& strSection) const;
		bool UpdateEntry(Config config, const std::string& strSection, const std::string& strKey, const std::optional<std::string>& value, const std::string& oldKey = {});

	  private:
		std::unordered_map<Config, std::unique_ptr<CIniFile>> m_iniFiles;
	};
} // namespace ini

#endif
