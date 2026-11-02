#pragma once
#include "../trade/CBrokerClient.h"
#include "AccountInfo.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

class CAccountService final
{
	friend class CServiceRoutingTests;
	friend class CBrokerClientTests;

public:
	using _TyAccountStateHandler = std::function<void(const _TyAccount&)>;
	using _TyPushHandler = std::function<void(const CRequest&)>;
	explicit CAccountService(CBrokerClient* pBroker);
	~CAccountService();
	bool Initialize();
	void Stop();
	bool HasAccount(const _TyAccount& accountIndex) const;
	bool IsConnected(const _TyAccount& accountIndex) const;
	bool AcquireAccount(const _TyAccount& accountIndex);
	void ReleaseAccount(const _TyAccount& accountIndex);
	std::vector<_TyAccount> GetAccounts() const;
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	void SetPushHandler(_TyPushHandler&& handler);
	void SetAccountStateHandler(_TyAccountStateHandler&& handler);
	void OnBrokerEvent(const trade::CCountEvent& event);
	void OnBrokerState(SessionState state);
	const std::string& GetLastError() const;

private:
	void NotifyState(const CAccountInfo& info);
	void RefreshAccounts();
	void OnCatalogue(const CRequest& response, std::uint64_t nGeneration);
	CBrokerClient* m_pBroker{ nullptr };
	mutable std::mutex m_mtx_accounts;
	std::map<_TyAccount, CAccountInfo> m_accounts;
	std::map<_TyAccount, std::size_t> m_accountUsers;
	std::set<_TyAccount> m_disconnectingAccounts;
	_TyPushHandler m_pushHandler;
	_TyAccountStateHandler m_accountStateHandler;
	std::uint64_t m_generation{ 0 };
	std::atomic_bool m_bStopping{ false };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;
	std::string m_strLastError;
};
