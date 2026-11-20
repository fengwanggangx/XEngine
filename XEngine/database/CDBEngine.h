#ifndef __CDBENGINE_H__
#define __CDBENGINE_H__

#include "../common/ISingleton.h"
#include "CODBC.h"

class CDBEngine final : public ISingleton<CDBEngine>
{
	DECLARE_SINGLE_DFAULT(CDBEngine)
public:
	int Initialize(db::em_database ty, const db::CConnectParam& param, int nCount = 1);
	int Close();
	int Close(db::em_database t);
	db::_TyDBPtr GetDBPtr(db::em_database t);
	db::_TyDBPtr GetDBPtr(const std::string& strType);
	std::size_t Count(db::em_database t) const;

private:
	std::unique_ptr<db::CODBC> m_odbc{ nullptr };
};
#endif
