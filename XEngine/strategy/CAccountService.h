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
	using _TyAccountStateHandler = std::function<void(const _TyAccount&)>;

	explicit CAccountService(CSession* pSession);
	~CAccountService();
	bool Initialize();
	void Stop();
	bool HasAccount(const _TyAccount& account) const;
	bool IsConnected(const _TyAccount& account) const;
	bool AcquireAccount(const _TyAccount& account);
	void ReleaseAccount(const _TyAccount& account);
	std::vector<_TyAccount> GetAccounts() const;
	bool HandleAccountRequest(const CRequest& req, const std::string& actor);
	void SetPushHandler(_TyRequestNotify&& handler);
	void SetAccountStateHandler(_TyAccountStateHandler&& handler);
	void OnBrokerEvent(const trade::CCounterEvent& event);
	void OnBrokerState(SessionState state, const std::string& reason = {});
	const std::string& GetLastError() const;

private:
	void NotifyState(const CAccountInfo& info);
	void RefreshAccounts();
	void OnCatalogue(const CRequest& response, std::uint64_t nGeneration);

	void FailPending();
	void RefreshPending();
	bool OnRequestReply(const CRequest& req);
	bool SendRequest(CRequest&& request, _TyRequestNotify&& handler);

	struct CAccountRuntime
	{
		CAccountInfo m_info;
		std::size_t m_useCount{ 0 };
		bool m_bListed{ true };
		bool m_bDisconnecting{ false };
	};

	void FinishDisconnect(const _TyAccount& account);

	struct CPending
	{
		_TyRequestNotify m_handler;
		std::chrono::steady_clock::time_point m_deadline;
		_TyRequest m_type{ _TyRequest::UNKNOWN };
	};

	std::mutex m_mtx_requests;
	std::unordered_map<_TyRequestId, CPending> m_pending;
	CSession* m_pSession{ nullptr };
	mutable std::shared_mutex m_mtx_accounts;
	std::map<_TyAccount, CAccountRuntime> m_accounts;
	_TyRequestNotify m_pushHandler;
	_TyAccountStateHandler m_accountStateHandler;
	std::uint64_t m_generation{ 0 };
	std::atomic_bool m_bStopping{ false };
	std::jthread m_refresh;
	std::mutex m_mtx_wait;
	std::condition_variable m_wake;
	std::string m_strLastError;
};
