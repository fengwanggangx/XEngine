#ifndef __CINIFILE_H__
#define __CINIFILE_H__

#include <SimpleIni/SimpleIni.h>
#include <atomic>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace ini
{
	class CIniFile
	{
	  public:
		explicit CIniFile(const std::string& strFile);
		~CIniFile();
		std::vector<std::pair<std::string, std::string>> GetSection(const std::string& strSection) const;
		bool UpdateEntry(const std::string& strSection, const std::string& strKey, const std::optional<std::string>& value, const std::string& oldKey = {});

	  public:
		int GetInt(const std::string& strSection, const std::string& strKey, int nDefault = -1) const;
		bool GetBool(const std::string& strSection, const std::string& strKey, bool bDefault = false) const;
		double GetDouble(const std::string& strSection, const std::string& strKey, double fDefault = 0.0) const;
		std::string GetString(const std::string& strSection, const std::string& strKey, const std::string& strDefault = "") const;

		bool SetInt(const std::string& strSection, const std::string& strKey, int nVal);
		bool SetBool(const std::string& strSection, const std::string& strKey, bool bVal);
		bool SetDouble(const std::string& strSection, const std::string& strKey, double fVal);
		bool SetString(const std::string& strSection, const std::string& strKey, const std::string& strVal);

		int GetValue(const std::string& strSection, const std::string& strKey, int nDefault) const;
		bool GetValue(const std::string& strSection, const std::string& strKey, bool bDefault) const;
		double GetValue(const std::string& strSection, const std::string& strKey, double fDefault) const;
		std::string GetValue(const std::string& strSection, const std::string& strKey, const std::string& strDefault) const;

		bool SetValue(const std::string& strSection, const std::string& strKey, int nVal);
		bool SetValue(const std::string& strSection, const std::string& strKey, bool bVal);
		bool SetValue(const std::string& strSection, const std::string& strKey, double fVal);
		bool SetValue(const std::string& strSection, const std::string& strKey, const std::string& strVal);

	  private:
		bool IsSectionExists(const std::string& strSection) const;
		std::vector<std::string> GetSections() const;

	  private:
		bool Load(const std::string& strFile);
		bool Save() const;

	  private:
		mutable std::shared_mutex m_mtx_parser;
		std::unique_ptr<CSimpleIniA> m_pParser;
		std::string m_strFileName;
		std::atomic_bool m_bUpdated{false};
	};
} // namespace ini

#endif
