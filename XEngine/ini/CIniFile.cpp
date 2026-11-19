#include "CIniFile.h"
#include <mutex>

namespace ini
{
	std::vector<std::pair<std::string, std::string>> CIniFile::GetSection(const std::string& strSection) const
	{
		CSimpleIniA::TNamesDepend keys;
		std::vector<std::pair<std::string, std::string>> entries;

		{
			std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
			m_pParser->GetAllKeys(strSection.c_str(), keys);
			keys.sort(CSimpleIniA::Entry::LoadOrder());
			entries.reserve(keys.size());
			for (const auto& strKey : keys)
			{
				entries.emplace_back(strKey.pItem, m_pParser->GetValue(strSection.c_str(), strKey.pItem, ""));
			}
		}
		return entries;
	}

	bool CIniFile::UpdateEntry(const std::string& strSection, const std::string& strKey, const std::optional<std::string>& value, const std::string& oldKey)
	{
		if (strSection.empty() || strKey.empty())
		{
			return false;
		}
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		std::string data;
		std::unique_ptr<CSimpleIniA> parser = std::make_unique<CSimpleIniA>(false, false, false);
		if (0 > m_pParser->Save(data) || 0 > parser->LoadData(data))
		{
			return false;
		}
		if (value.has_value())
		{
			if (0 > parser->SetValue(strSection.c_str(), strKey.c_str(), value->c_str()))
			{
				return false;
			}
			if (!oldKey.empty())
			{
				parser->Delete(strSection.c_str(), oldKey.c_str());
			}
		}
		else
		{
			parser->Delete(strSection.c_str(), strKey.c_str());
		}
		if (0 > parser->SaveFile(m_str_file_name.c_str()))
		{
			return false;
		}
		m_pParser.swap(parser);
		m_bUpdated = false;
		return true;
	}

	CIniFile::CIniFile(const std::string& strFile) : m_pParser(std::make_unique<CSimpleIniA>(false, false, false)), m_str_file_name(strFile)
	{
		Load(strFile);
	}

	CIniFile::~CIniFile()
	{
		Save();
	}

	bool CIniFile::Load(const std::string& strFile)
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		SI_Error ret = m_pParser->LoadFile(strFile.c_str());
		if (0 > ret)
		{
			if (SI_FILE == ret)
			{
				return 0 <= m_pParser->SaveFile(strFile.c_str());
			}
			return false;
		}
		return true;
	}

	bool CIniFile::Save() const
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		SI_Error ret = m_pParser->SaveFile(m_str_file_name.c_str());
		return 0 <= ret;
	}

	int CIniFile::GetInt(const std::string& strSection, const std::string& strKey, int nDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
		return m_pParser->GetLongValue(strSection.c_str(), strKey.c_str(), nDefault);
	}

	bool CIniFile::GetBool(const std::string& strSection, const std::string& strKey, bool bDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
		return m_pParser->GetBoolValue(strSection.c_str(), strKey.c_str(), bDefault);
	}

	double CIniFile::GetDouble(const std::string& strSection, const std::string& strKey, double fDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
		return m_pParser->GetDoubleValue(strSection.c_str(), strKey.c_str(), fDefault);
	}

	std::string CIniFile::GetString(const std::string& strSection, const std::string& strKey, const std::string& strDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
		return m_pParser->GetValue(strSection.c_str(), strKey.c_str(), strDefault.c_str());
	}

	bool CIniFile::SetInt(const std::string& strSection, const std::string& strKey, int nVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		bool bRet = SI_FAIL != m_pParser->SetLongValue(strSection.c_str(), strKey.c_str(), nVal);
		m_bUpdated = m_bUpdated || bRet;
		return bRet;
	}

	bool CIniFile::SetBool(const std::string& strSection, const std::string& strKey, bool bVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		bool bRet = SI_FAIL != m_pParser->SetBoolValue(strSection.c_str(), strKey.c_str(), bVal);
		m_bUpdated = m_bUpdated || bRet;
		return bRet;
	}

	bool CIniFile::SetDouble(const std::string& strSection, const std::string& strKey, double fVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		bool bRet = SI_FAIL != m_pParser->SetDoubleValue(strSection.c_str(), strKey.c_str(), fVal);
		m_bUpdated = m_bUpdated || bRet;
		return bRet;
	}

	bool CIniFile::SetString(const std::string& strSection, const std::string& strKey, const std::string& strVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_mtx_parser);
		bool bRet = SI_FAIL != m_pParser->SetValue(strSection.c_str(), strKey.c_str(), strVal.c_str());
		m_bUpdated = m_bUpdated || bRet;
		return bRet;
	}

	int CIniFile::GetValue(const std::string& strSection, const std::string& strKey, int nDefault) const
	{
		return GetInt(strSection, strKey, nDefault);
	}

	bool CIniFile::GetValue(const std::string& strSection, const std::string& strKey, bool bDefault) const
	{
		return GetBool(strSection, strKey, bDefault);
	}

	double CIniFile::GetValue(const std::string& strSection, const std::string& strKey, double fDefault) const
	{
		return GetDouble(strSection, strKey, fDefault);
	}

	std::string CIniFile::GetValue(const std::string& strSection, const std::string& strKey, const std::string& strDefault) const
	{
		return GetString(strSection, strKey, strDefault);
	}

	bool CIniFile::SetValue(const std::string& strSection, const std::string& strKey, int nVal)
	{
		return SetInt(strSection, strKey, nVal);
	}

	bool CIniFile::SetValue(const std::string& strSection, const std::string& strKey, bool bVal)
	{
		return SetBool(strSection, strKey, bVal);
	}

	bool CIniFile::SetValue(const std::string& strSection, const std::string& strKey, double fVal)
	{
		return SetDouble(strSection, strKey, fVal);
	}

	bool CIniFile::SetValue(const std::string& strSection, const std::string& strKey, const std::string& strVal)
	{
		return SetString(strSection, strKey, strVal);
	}

	bool CIniFile::IsSectionExists(const std::string& strSection) const
	{
		std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
		CSimpleIniA::TNamesDepend sections;
		m_pParser->GetAllSections(sections);

		for (const auto& s : sections)
		{
			if (strSection == s.pItem)
			{
				return true;
			}
		}
		return false;
	}

	std::vector<std::string> CIniFile::GetSections() const
	{
		std::vector<std::string> ret;
		CSimpleIniA::TNamesDepend sections;

		{
			std::shared_lock<std::shared_mutex> lck(m_mtx_parser);
			m_pParser->GetAllSections(sections);
		}

		for (const auto& s : sections)
		{
			ret.emplace_back(s.pItem);
		}
		return ret;
	}
} // namespace ini
