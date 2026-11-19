#include "utility.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <random>

namespace utility
{
	std::string ToHex(const std::string& strValue)
	{
		constexpr std::array<char, 16> digits{ '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
		std::string result;
		result.reserve(strValue.size() * 2);
		for (unsigned char character : strValue)
		{
			result.push_back(digits[character >> 4]);
			result.push_back(digits[character & 0x0F]);
		}
		return result;
	}

	std::string MakeSaltHex()
	{
		std::random_device randomDevice;
		std::array<unsigned char, 16> salt;
		for (unsigned char& value : salt)
		{
			value = static_cast<unsigned char>(randomDevice());
		}
		return ToHex(std::string(reinterpret_cast<const char*>(salt.data()), salt.size()));
	}

	std::string Utf8Literal(const std::string& strValue)
	{
		return "CONVERT(UNHEX('" + ToHex(strValue) + "') USING utf8mb4)";
	}

	std::string lower(std::string strVal)
	{
		std::transform(strVal.begin(), strVal.end(), strVal.begin(), [](unsigned char ch)
		{
			return static_cast<char>(std::tolower(ch));
		});
		return strVal;
	}

	size_t split(const std::string& s, std::vector<std::string>& v, char delim, bool bEmpty)
	{
		v.clear();
		const char* p = s.c_str();
		size_t n = strlen(p);
		size_t opos = 0;
		for (size_t i = 0; i < n; ++i)
		{
			bool bDelim = (*(p + i) == delim);
			bool bEnd = i >= (n - 1);
			if (bDelim)
			{
				if ((i - opos > 0) || bEmpty)
				{
					v.emplace_back(s.substr(opos, i - opos));
				}
				opos = bEnd ? i : i + 1;
			}
			if (bEnd)
			{
				if (bDelim)
				{
					if (bEmpty)
					{
						v.emplace_back(s.substr(opos, 0));
					}
				}
				else
				{
					v.emplace_back(s.substr(opos, -1));
				}
			}
		}
		return v.size();
	}

	std::size_t split(const std::string& s, std::vector<std::string_view>& v, char delim, bool bEmpty)
	{
		std::size_t nLength = s.length();
		if (nLength <= 0)
		{
			v.clear();
			return 0;
		}
		const char* p = s.c_str();
		std::size_t sz = v.size();
		std::size_t n = 0;
		std::size_t nStart = 0;
		for (std::size_t i = 0; i < nLength; ++i)
		{
			if (s[i] == delim)
			{
				if ((!bEmpty) && (nStart == i))
				{
					nStart = i + 1;
					continue;
				}
				if (n < sz)
				{
					v.at(n) = std::string_view(p + nStart, i - nStart);
				}
				else
				{
					v.emplace_back(p + nStart, i - nStart);
				}
				++n;
				nStart = i + 1;
			}
		}
		if (n < sz)
		{
			v.at(n) = std::string_view(p + nStart, nLength - nStart);
		}
		else
		{
			v.emplace_back(p + nStart, nLength - nStart);
		}
		++n;
		if (n < sz)
		{
			v.resize(n);
		}
		return n;
	}

} // namespace utility
