#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <iomanip>
#include <sstream>

using _TyAccountIndex = std::tuple<std::uint64_t, std::string, std::string>;

inline bool IsAccountValid(const _TyAccountIndex& account)
{
	return 0 != std::get<0>(account) && !std::get<1>(account).empty() && !std::get<2>(account).empty();
}

inline std::string EncodeAccount(const _TyAccountIndex& account)
{
	std::ostringstream stream;
	stream << std::get<0>(account) << ' ' << std::quoted(std::get<1>(account)) << ' ' << std::quoted(std::get<2>(account));
	return stream.str();
}

inline bool DecodeAccount(const std::string& text, _TyAccountIndex& account)
{
	std::istringstream stream(text);
	_TyAccountIndex parsed;
	if (!(stream >> std::get<0>(parsed) >> std::quoted(std::get<1>(parsed)) >> std::quoted(std::get<2>(parsed))))
	{
		return false;
	}
	stream >> std::ws;
	if (!stream.eof() || !IsAccountValid(parsed))
	{
		return false;
	}
	account = std::move(parsed);
	return true;
}

template <class T>
_TyAccountIndex AccountIndex(const T& account)
{
	return { account.broker_id(), account.site(), account.account() };
}

template <class T>
void SetAccountIdentity(const _TyAccountIndex& account, T& target)
{
	target.set_broker_id(std::get<0>(account));
	target.set_site(std::get<1>(account));
	target.set_account(std::get<2>(account));
}
