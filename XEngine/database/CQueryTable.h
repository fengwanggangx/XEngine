#ifndef __CQUERYTABLE_H__
#define __CQUERYTABLE_H__

#include "common_db.h"
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace db
{
	using _TyCell = std::variant<std::monostate, std::int64_t, double, std::string, std::vector<std::uint8_t>>;
	using _TyRow = std::vector<_TyCell>;

	struct CColumn
	{
		unsigned int m_uId{ 0 };
		std::string m_strName;
		em_data_types m_type{ em_data_types::em_string };
		int m_nDecimal{ -1 };
	};

	class CTable final
	{
	  public:
		using _TyColumns = std::vector<CColumn>;
		using _TyRows = std::vector<_TyRow>;

		void Clear()
		{
			m_columns.clear();
			m_rows.clear();
		}

		bool IsEmpty() const
		{
			return m_columns.empty() || m_rows.empty();
		}

		_TyColumns m_columns;
		_TyRows m_rows;
	};

	inline std::string to_string(const _TyCell& value)
	{
		if (const std::string* pValue = std::get_if<std::string>(&value))
		{
			return *pValue;
		}
		if (const std::int64_t* pValue = std::get_if<std::int64_t>(&value))
		{
			return std::to_string(*pValue);
		}
		if (const double* pValue = std::get_if<double>(&value))
		{
			return std::to_string(*pValue);
		}
		if (const std::vector<std::uint8_t>* pValue = std::get_if<std::vector<std::uint8_t>>(&value))
		{
			return std::string(pValue->begin(), pValue->end());
		}
		return {};
	}

	inline std::size_t to_string_row(const _TyRow& row, std::vector<std::string>& val)
	{
		val.clear();
		for (const db::_TyCell& v : row)
		{
			val.emplace_back(db::to_string(v));
		}
		return val.size();
	}

} // namespace db

#endif
