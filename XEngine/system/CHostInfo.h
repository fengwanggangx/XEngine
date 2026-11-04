#ifndef XENGINE_SYSTEM_CHOSTINFO_H
#define XENGINE_SYSTEM_CHOSTINFO_H

#include <string>

struct CHostInfo
{
	std::string m_strName;
	std::string m_strHost;
	unsigned int m_nPort{ 0 };

	bool Valid() const
	{
		return !m_strName.empty() && !m_strHost.empty() && (0 < m_nPort) && (65535 >= m_nPort);
	}
};

#endif
