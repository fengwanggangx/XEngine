#ifndef __CORACLE_H__
#define __CORACLE_H__

#include "IDataBase.h"

namespace db
{
	class COracle : public IDataBase
	{
	public:
		COracle();
		~COracle();

	public:
		int Connect(const CConnectParam& param) override;
		int Close() override;

		int ExecUpdate(const std::string& strSQL) override;
		const CTable& ExecQuery(const std::string& strSQL) override;

		bool BeginTransaction() override;
		bool EndTransaction() override;
		bool RollBackTransaction() override;

	private:
		void* m_db{ nullptr };
	};
} // namespace db
#endif
