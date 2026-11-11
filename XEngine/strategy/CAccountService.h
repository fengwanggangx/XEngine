#pragma once
#include "../system/CSession.h"
#include "../trade/TradeTypes.h"
#include "AccountInfo.h"
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <unordered_map>

class CAccountService final
{
	friend class CServiceRoutingTests;
	friend class CBrokerServiceTests;

public:
	bool OnReply(const CRequest& req);
	using _TyAccountStateHandler = std::function<void(const _TyAccount&)>;

	void Stop();
	bool Initialize();
	~CAccountService();
	std::vector<_TyAccount> GetAccounts() const;
	CAccountInfo GetAccountInfo(const _TyAccount& account) const;
	explicit CAccountService(CSession* pSession);
	bool AcquireAccount(const _TyAccount& account);
	void ReleaseAccount(const _TyAccount& account);
	void SetPushHandler(_TyRequestNotify&& handler);
	bool HasAccount(const _TyAccount& account) const;
	bool IsConnected(const _TyAccount& account) const;
	bool IsInUse(const _TyAccount& account) const;
	void OnBrokerEvent(const trade::CCounterEvent& event);
	bool OnRequest(const CRequest& req, const std::string& actor);
	void SetAccountStateHandler(_TyAccountStateHandler&& handler);
	void OnBrokerState(SessionState state, const std::string& reason = {});
	const std::string& GetLastError() const;

private:
	void RefreshAccounts();
	void NotifyState(const CAccountInfo& info);
	void OnAccountListReply(const CRequest& response, std::uint64_t brokerConnectId);

	void FailPending();
	void RefreshPending();
	bool SendRequest(CRequest&& request);

	struct CAccountRuntime
	{
		CAccountInfo m_info;
		std::size_t m_useCount{ 0 };
		bool m_bAtAccountList{ true };
		bool m_bDisconnecting{ false };
	};

	void FinishDisconnect(const _TyAccount& account);
	bool OnAccountReply(const CRequest& origin, const CRequest& reply);

	struct CPending
	{
		CRequest m_origin;
		std::chrono::steady_clock::time_point m_deadline;
	};

	bool DispatchReply(const CRequest& origin, const CRequest& reply);

	std::shared_mutex m_mtx_requests;
	std::unordered_map<_TyRequestId, CPending> m_pending;
	CSession* m_pSession{ nullptr };
	mutable std::shared_mutex m_mtx_accounts;
	std::map<_TyAccount, CAccountRuntime> m_accounts;
	_TyRequestNotify m_pushHandler;
	_TyAccountStateHandler m_accountStateHandler;
	std::uint64_t m_broker_connect_id{ 0 };
	std::atomic_bool m_bStopping{ false };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;
	std::string m_strLastError;
};
