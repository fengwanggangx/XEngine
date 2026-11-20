#ifndef XENGINE_SYSTEM_CHOSTINFO_H
#define XENGINE_SYSTEM_CHOSTINFO_H

#include <string>

struct CHostInfo
{
	std::string m_name;
	std::string m_host;
	unsigned int m_port{ 0 };

	bool Valid() const
	{
		return !m_name.empty() && !m_host.empty() && (0 < m_port) && (65535 >= m_port);
	}
};

#endif
