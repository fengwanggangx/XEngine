#ifndef __CSQLITE3_H__
#define __CSQLITE3_H__

#include "IDataBase.h"

namespace db
{
	class CSQLite3 : public IDataBase
	{
	public:
		CSQLite3();
		~CSQLite3();

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
