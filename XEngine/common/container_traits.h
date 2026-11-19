#pragma once
#ifndef _CONTAINER_TRAITS_H_
#define _CONTAINER_TRAITS_H_
#include <set>
#include <deque>
#include <map>
#include <list>
#include <unordered_set>

/*
//SFINAE 版本，非正式的采用编译重载决议的取巧实现，不同编译器可能行为表现不一致，不推荐
template <class _Ty>
struct is_container_sfinae
{
template<class _P>
static auto check(_P* p) -> decltype(
std::begin(*p),
std::end(*p),
std::true_type {}
);

template<typename>
static std::false_type check(...);

static constexpr bool value = decltype(check<_Ty>(nullptr))::value;
};

*/

namespace traits
{
	template <class _Ty>
	struct is_container
	{
		static constexpr bool value = false;
	};

	template <class _Ty, class _Alloc>
	struct is_container<std::vector<_Ty, _Alloc>>
	{
		static constexpr bool value = true;
	};

	template <class _Kty, class _Ty, class _Pr, class _Alloc>
	struct is_container<std::map<_Kty, _Ty, _Pr, _Alloc>>
	{
		static constexpr bool value = true;
	};

	template <class _K, class _Ty>
	struct is_container<std::unordered_map<_K, _Ty>>
	{
		static constexpr bool value = true;
	};

	template <class _Ty, class _Alloc>
	struct is_container<std::list<_Ty, _Alloc>>
	{
		static constexpr bool value = true;
	};

	template <class _Kty, class _Pr, class _Alloc>
	struct is_container<std::set<_Kty, _Pr, _Alloc>>
	{
		static constexpr bool value = true;
	};

	template <class _Ty, class _Alloc>
	struct is_container<std::deque<_Ty, _Alloc>>
	{
		static constexpr bool value = true;
	};

	template <typename T>
	struct is_associative_container : std::false_type
	{
	};

	template <typename... Args>
	struct is_associative_container<std::map<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_associative_container<std::unordered_map<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_associative_container<std::set<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_associative_container<std::unordered_set<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_associative_container<std::multiset<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_associative_container<std::unordered_multiset<Args...>> : std::true_type
	{
	};

	template <typename T>
	struct is_sequence_container : std::false_type
	{
	};

	template <typename... Args>
	struct is_sequence_container<std::vector<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_sequence_container<std::array<Args...>> : std::true_type
	{
	};

	template <typename... Args>
	struct is_sequence_container<std::deque<Args...>> : std::true_type
	{
	};
}; // namespace traits
#endif