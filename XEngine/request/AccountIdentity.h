#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <iomanip>
#include <sstream>

using _TyAccount = std::pair<std::uint64_t, std::string>;

struct CAccountHash
{
	std::size_t operator()(const _TyAccount& account) const noexcept
	{
		std::size_t brokerHash = std::hash<std::uint64_t>{}(account.first);
		std::size_t accountHash = std::hash<std::string>{}(account.second);
		return brokerHash ^ (accountHash + 0x9e3779b9U + (brokerHash << 6) + (brokerHash >> 2));
	}
};

inline bool IsAccountValid(const _TyAccount& account)
{
	return 0 != account.first && !account.second.empty();
}

inline std::string EncodeAccount(const _TyAccount& account)
{
	std::ostringstream stream;
	stream << account.first << ' ' << std::quoted(account.second);
	return stream.str();
}

inline bool DecodeAccount(const std::string& text, _TyAccount& account)
{
	std::istringstream stream(text);
	_TyAccount parsed;
	if (!(stream >> parsed.first >> std::quoted(parsed.second)))
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
_TyAccount FmtAccount(const T& account)
{
	return { account.broker_id(), account.account() };
}

template <class T>
void SetAccountIdentity(const _TyAccount& account, T& target)
{
	target.set_broker_id(account.first);
	target.set_account(account.second);
}
