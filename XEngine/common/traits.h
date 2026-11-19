#pragma once
#ifndef _TRAITS_H_
#define _TRAITS_H_

namespace traits
{
	template <typename... _Ty>
	struct void_t_impl
	{
		using type = void;
	};

	template <typename... _Ty>
	using _TyVoid = typename void_t_impl<_Ty...>::type;

	template <typename _Ret, typename _Fn, typename = void, typename... _Args>
	struct is_invocable : std::false_type
	{
	};

	template <typename _Ret, typename _Fn, typename... _Args>
	struct is_invocable<_Ret, _Fn, _TyVoid<decltype(std::declval<_Fn>()(std::declval<_Args>()...))>, _Args...> : std::is_convertible<typename std::result_of<_Fn(_Args...)>::type, _Ret>
	{
	};

	template <typename _Ret, typename _Fn, typename... _Args>
	constexpr bool is_invocable_v = traits::is_invocable<_Ret, _Fn, void, _Args...>::value;
} // namespace traits

#endif