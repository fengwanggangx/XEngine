#pragma once
/**********************************************************
 * @Date   : 2025.12.26
 * @Author : fengwanggang
 * @desc   : 主要用于容器操作
 **********************************************************/

#ifndef _CONTAINER_UTILITY_H_
#define _CONTAINER_UTILITY_H_

#include "traits.h"
#include "container_traits.h"

namespace container
{
	// 删除容器索引数据
	template <class _TyContainer, class _TyIdx, typename std::enable_if_t<traits::is_sequence_container<_TyIdx>::value, int> = 0>
	void remove_idx(_TyContainer& container, const _TyIdx& idx)
	{
		using _TyItem = _TyContainer::value_type;
		const auto vIter = std::remove_if(container.begin(), container.end(), [&, i = 0](const _TyItem&) mutable
		{
			return std::find(idx.begin(), idx.end(), i++) != idx.end();
		});
		container.erase(vIter, container.end());
	}

	template <class _TyContainer, class _TyIdx, typename std::enable_if_t<traits::is_associative_container<_TyIdx>::value, int> = 0>
	void remove_idx(_TyContainer& container, const _TyIdx& idx)
	{
		using _TyItem = _TyContainer::value_type;
		const auto vIter = std::remove_if(container.begin(), container.end(), [&, i = 0](const _TyItem&) mutable
		{
			return idx.find(i++) != idx.end();
		});
		container.erase(vIter, container.end());
	}

	template <class _TyContainer, class _Deletor, typename std::enable_if_t<traits::is_container<_TyContainer>::value, int> = 0>
	void remove_at(_TyContainer& container, _Deletor&& del)
	{
		const auto vIter = std::remove_if(container.begin(), container.end(), std::forward<_Deletor>(del));
		container.erase(vIter, container.end());
	}

	template <class _TyContainer, class _TyKey, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value, int> = 0>
	bool with(const _TyContainer& container, const _TyKey& key)
	{
		const auto mIter = container.find(key);
		return (mIter != container.end());
	}

	template <class _TyContainer, class _TyKey, typename std::enable_if_t<traits::is_sequence_container<_TyContainer>::value, int> = 0>
	bool with(const _TyContainer& container, const _TyKey& key)
	{
		const auto mIter = std::find(container.begin(), container.end(), key);
		return (mIter != container.end());
	}

	template <class _TyContainer, class _TyKey, class _Cmp>
	bool with(const _TyContainer& container, const _TyKey& key, _Cmp&& func)
	{
		for (const auto& v : container)
		{
			if (func(v, key))
			{
				return true;
			}
		}
		return false;
	}

	template <class _TyContainer, class _TyKey = typename _TyContainer::key_type, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value, int> = 0>
	const auto& vfind(const _TyContainer& container, const _TyKey& key)
	{
		const auto mIter = container.find(key);
		if (mIter != container.end())
		{
			return mIter->second;
		}

		using _TyRet = _TyContainer::mapped_type;
		thread_local _TyRet s;
		return s;
	}

	template <class _TyContainer, class _TyVal = typename _TyContainer::value_type, typename std::enable_if_t<traits::is_sequence_container<_TyContainer>::value, int> = 0>
	const auto& vfind(const _TyContainer& container, const _TyVal& key)
	{
		const auto mIter = std::find(container.begin(), container.end(), key);
		if (mIter != container.end())
		{
			return *mIter;
		}

		thread_local _TyVal s;
		return s;
	}

	template <class _TyContainer, class _TyKey, class _TyVal = typename _TyContainer::mapped_type, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value, int> = 0>
	const auto& vfind(const _TyContainer& container, const _TyKey& key, const _TyVal& def)
	{
		const auto mIter = container.find(key);
		if (mIter != container.end())
		{
			return mIter->second;
		}
		thread_local _TyVal s;
		s = def;
		return s;
	}

	template <class _TyContainer, class _TyKey, class _TyVal = typename _TyContainer::value_type, typename std::enable_if_t<traits::is_sequence_container<_TyContainer>::value, int> = 0>
	const auto& vfind(const _TyContainer& container, const _TyKey& key, const _TyVal& def)
	{
		const auto mIter = std::find(container.begin(), container.end(), key);
		if (mIter != container.end())
		{
			return mIter->second;
		}

		thread_local _TyVal s;
		s = def;
		return s;
	}

	template <class _TyContainer, class _TyKey, class _TyVal = typename _TyContainer::mapped_type, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value, int> = 0>
	bool try_vfind(const _TyContainer& container, const _TyKey& key, _TyVal& val)
	{
		const auto mIter = container.find(key);
		if (mIter != container.end())
		{
			val = mIter->second;
			return true;
		}

		thread_local _TyVal s;
		val = s;
		return false;
	}

	template <class _TyContainer, class _TyKey, class _TyVal = typename _TyContainer::value_type, typename std::enable_if_t<traits::is_sequence_container<_TyContainer>::value, int> = 0>
	bool try_vfind(const _TyContainer& container, const _TyKey& key, _TyVal& val)
	{
		const auto mIter = std::find(container.begin(), container.end(), key);
		if (mIter != container.end())
		{
			val = mIter->second;
			return true;
		}
		thread_local _TyVal s;
		val = s;
		return false;
	}

	template <class _TyContainer, class _TyKey, class _Creator, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value && traits::is_invocable_v<typename _TyContainer::mapped_type, _Creator>, int> = 0>
	const auto& vfind(_TyContainer& container, const _TyKey& key, _Creator&& creator)
	{
		const auto mIter = container.find(key);
		if (mIter != container.end())
		{
			return mIter->second;
		}

		// using _TyItem = typename _TyContainer::mapped_type;
		//_vTy v = std::forward<_Creator>(creator)();
		container.emplace(key, std::forward<_Creator>(creator)());
		return vfind(container, key);
	}

	template <class _TyContainer, class _TyKey, class _Creator, typename std::enable_if_t<traits::is_sequence_container<_TyContainer>::value && traits::is_invocable_v<typename _TyContainer::mapped_type, _Creator>, int> = 0>
	auto& vfind(_TyContainer& container, const _TyKey& key, _Creator&& creator)
	{
		auto iter = std::find(container.begin(), container.end(), key);
		if (iter != container.end())
		{
			return iter->second;
		}

		// using _TyItem = typename _TyContainer::value_type;
		//_vTy v = std::forward<_Creator>(creator)();
		container.push_back(std::forward<_Creator>(creator)());
		return vfind(container, key);
	}

	template <class _TyContainer, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value, int> = 0>
	void vcopy(_TyContainer& dest, const _TyContainer& src, bool bReplace)
	{
		for (const auto& v : src)
		{
			auto ret = dest.emplace(v);
			if (bReplace && !ret.second)
			{
				ret.first->second = v.second;
			}
		}
	}

	template <class _TyContainer0, class _TyContainer1, typename std::enable_if_t<traits::is_sequence_container<_TyContainer0>::value && traits::is_associative_container<_TyContainer1>::value, int> = 0>
	void vcopy(_TyContainer0& dest, const _TyContainer1& src, bool bCopyKey)
	{
		for (const auto& v : src)
		{
			dest.emplace_back(bCopyKey ? v.first : v.second);
		}
	}

	template <class _TyContainer0, class _TyContainer1, class _Fn>
	void vcopy(_TyContainer0& dest, const _TyContainer1& src, _Fn&& func)
	{
		for (const auto& v : src)
		{
			func(dest, v);
		}
	}

	template <class _TyContainer, class... _Args, typename std::enable_if_t<traits::is_sequence_container<_TyContainer>::value, int> = 0>
	auto& emplace_back(_TyContainer& container, _Args&&... args)
	{
		container.emplace_back(std::forward<_Args>(args)...);
		return container.back();
	}

	template <class _TyContainer, class... _Args, typename std::enable_if_t<traits::is_associative_container<_TyContainer>::value, int> = 0>
	auto& emplace_back(_TyContainer& container, _Args&&... args)
	{
		return container.emplace_back(std::forward<_Args>(args)...)->second;
	}
}; // namespace container

#endif