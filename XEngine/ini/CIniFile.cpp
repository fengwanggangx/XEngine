#include "CIniFile.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>

namespace ini
{
	namespace
	{
		bool SaveAtomically(const CSimpleIniA& parser, const std::filesystem::path& path)
		{
			std::string contents;
			if (0 > parser.Save(contents))
			{
				return false;
			}
			std::error_code error;
			std::random_device random;
			std::filesystem::path temporary = path;
			temporary += ".tmp-" + std::to_string(random()) + "-" + std::to_string(random());
			if (!std::filesystem::create_directory(temporary, error))
			{
				return false;
			}
			std::filesystem::path staged = temporary / "config.ini";
			std::ofstream output(staged, std::ios::binary);
			output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
			output.close();
			bool saved = output.good();
			if (saved)
			{
				std::filesystem::rename(staged, path, error);
				saved = !error;
			}
			std::filesystem::remove(staged, error);
			std::filesystem::remove(temporary, error);
			return saved;
		}
	} // namespace

	std::vector<std::pair<std::string, std::string>> CIniFile::GetSection(const std::string& strSection) const
	{
		CSimpleIniA::TNamesDepend keys;
		std::vector<std::pair<std::string, std::string>> entries;

		{
			std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
			m_parser->GetAllKeys(strSection.c_str(), keys);
			keys.sort(CSimpleIniA::Entry::LoadOrder());
			entries.reserve(keys.size());
			for (const auto& strKey : keys)
			{
				entries.emplace_back(strKey.pItem, m_parser->GetValue(strSection.c_str(), strKey.pItem, ""));
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
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		std::string data;
		std::unique_ptr<CSimpleIniA> parser = std::make_unique<CSimpleIniA>(false, false, false);
		if (0 > m_parser->Save(data) || 0 > parser->LoadData(data))
		{
			return false;
		}
		if (value.has_value())
		{
			if (0 > parser->SetValue(strSection.c_str(), strKey.c_str(), value->c_str()))
			{
				return false;
			}
			SI_NoCase<char> less;
			if (!oldKey.empty() && (less(oldKey.c_str(), strKey.c_str()) || less(strKey.c_str(), oldKey.c_str())))
			{
				parser->Delete(strSection.c_str(), oldKey.c_str());
			}
		}
		else
		{
			parser->Delete(strSection.c_str(), strKey.c_str());
		}
		if (!SaveAtomically(*parser, m_file_name))
		{
			return false;
		}
		m_parser.swap(parser);
		m_updated = false;
		return true;
	}

	CIniFile::CIniFile(const std::string& strFile) : m_parser(std::make_unique<CSimpleIniA>(false, false, false)), m_file_name(strFile)
	{
		Load(strFile);
	}

	CIniFile::~CIniFile()
	{
		if (m_updated.load())
		{
			Save();
		}
	}

	bool CIniFile::Load(const std::string& strFile)
	{
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		SI_Error ret = m_parser->LoadFile(strFile.c_str());
		if (0 > ret)
		{
			std::error_code error;
			if ((SI_FILE == ret) && !std::filesystem::exists(strFile, error) && !error)
			{
				return SaveAtomically(*m_parser, strFile);
			}
			return false;
		}
		return true;
	}

	bool CIniFile::Save() const
	{
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		return SaveAtomically(*m_parser, m_file_name);
	}

	int CIniFile::GetInt(const std::string& strSection, const std::string& strKey, int nDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
		return m_parser->GetLongValue(strSection.c_str(), strKey.c_str(), nDefault);
	}

	bool CIniFile::GetBool(const std::string& strSection, const std::string& strKey, bool bDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
		return m_parser->GetBoolValue(strSection.c_str(), strKey.c_str(), bDefault);
	}

	double CIniFile::GetDouble(const std::string& strSection, const std::string& strKey, double fDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
		return m_parser->GetDoubleValue(strSection.c_str(), strKey.c_str(), fDefault);
	}

	std::string CIniFile::GetString(const std::string& strSection, const std::string& strKey, const std::string& strDefault) const
	{
		std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
		return m_parser->GetValue(strSection.c_str(), strKey.c_str(), strDefault.c_str());
	}

	bool CIniFile::SetInt(const std::string& strSection, const std::string& strKey, int nVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		bool bRet = 0 <= m_parser->SetLongValue(strSection.c_str(), strKey.c_str(), nVal);
		m_updated = m_updated || bRet;
		return bRet;
	}

	bool CIniFile::SetBool(const std::string& strSection, const std::string& strKey, bool bVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		bool bRet = 0 <= m_parser->SetBoolValue(strSection.c_str(), strKey.c_str(), bVal);
		m_updated = m_updated || bRet;
		return bRet;
	}

	bool CIniFile::SetDouble(const std::string& strSection, const std::string& strKey, double fVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		bool bRet = 0 <= m_parser->SetDoubleValue(strSection.c_str(), strKey.c_str(), fVal);
		m_updated = m_updated || bRet;
		return bRet;
	}

	bool CIniFile::SetString(const std::string& strSection, const std::string& strKey, const std::string& strVal)
	{
		std::unique_lock<std::shared_mutex> lck(m_parser_mutex);
		bool bRet = 0 <= m_parser->SetValue(strSection.c_str(), strKey.c_str(), strVal.c_str());
		m_updated = m_updated || bRet;
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
		std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
		CSimpleIniA::TNamesDepend sections;
		m_parser->GetAllSections(sections);

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
			std::shared_lock<std::shared_mutex> lck(m_parser_mutex);
			m_parser->GetAllSections(sections);
		}

		for (const auto& s : sections)
		{
			ret.emplace_back(s.pItem);
		}
		return ret;
	}
} // namespace ini
