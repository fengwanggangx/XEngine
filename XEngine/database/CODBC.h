#ifndef __CODBC_H__
#define __CODBC_H__

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>
#include <functional>
#include <shared_mutex>
#include "common_db.h"
#include "../common/defines.h"

namespace db
{
	class IDataBase;
	struct CConnectParam;

	using _TyDBReleasor = std::function<void(IDataBase*)>;
	using _TyDBPtr = std::unique_ptr<IDataBase, _TyDBReleasor>;

	struct CPoolState;

	class CODBC final
	{
		DECLARE_ONLY_CUSTOM_CONSTRUCT(CODBC)
	public:
		int Connect(db::em_database t, const CConnectParam& param, std::size_t nCount);
		int Close();
		int Close(db::em_database t);

		_TyDBPtr GetADataBase(db::em_database t);
		std::size_t Count(db::em_database t) const;

	private:
		std::shared_ptr<CPoolState> m_poolState;
	};

} // namespace db
#endif
