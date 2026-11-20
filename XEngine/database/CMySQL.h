#ifndef __CMySQL_H__
#define __CMySQL_H__
#include "IDataBase.h"
#include <mutex>
struct MYSQL;

namespace db
{
	class CMySQL : public IDataBase
	{
	public:
		CMySQL();
		~CMySQL();

	public:
		int Connect(const db::CConnectParam& param) override;
		int Close() override;

		int ExecUpdate(const std::string& strSQL) override;
		int ExecScript(const std::string& strSQL) override;
		const CTable& ExecQuery(const std::string& strSQL) override;

		bool BeginTransaction() override;
		bool EndTransaction() override;
		bool RollBackTransaction() override;

	private:
		void DisConnect();
		bool IsValid();

	private:
		std::mutex m_db_mutex;
		MYSQL* m_db{ nullptr };
	};
} // namespace db
#endif
