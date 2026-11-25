#ifndef XENGINE_HQMARKET_CSESSION_H
#define XENGINE_HQMARKET_CSESSION_H

#include "CHostInfo.h"
#include "../request/request.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct CLoginInfo
{
	std::string m_user;
	std::string m_password;
	std::string m_token;
	CHostInfo m_host;
	bool m_ordered{ false };

	bool Valid() const noexcept
	{
		return m_host.Valid() && (!m_token.empty() || (!m_user.empty() && !m_password.empty()));
	}
};

namespace net
{
	class CTcpClient;
	struct CNetEvent;
} // namespace net

enum class SessionState
{
	Disconnected,
	Connecting,
	Connected,
	Authenticating,
	Ready,
	Reconnecting,
	Stopping
};

class CSession final
{
	friend class CBrokerServiceTests;

public:
	using _TyStateHandler = std::function<void(SessionState, const std::string&)>;

	explicit CSession(const CLoginInfo& info);
	~CSession();
	CSession(const CSession&) = delete;
	CSession& operator=(const CSession&) = delete;

	bool Start();
	void Stop();
	void ClearHandlers();
	bool SendRequest(const CRequest& request);
	void RegisterHandler(_TyRequestNotify&& handler);
	void RegisterStateHandler(_TyStateHandler&& handler);
	void SetStateHandler(_TyStateHandler&& handler);
	SessionState GetState() const;
	bool IsConnected() const;
	bool IsAuthenticated() const;

private:
	void ConnectionLoop();
	void MaintenanceLoop();
	int OnClientNetEvent(const net::CNetEvent& ev);
	void HandleResponse(const CRequest& req);
	bool HandleAuthRequest(const CRequest& req);
	bool SendAuthentication();
	void NotifyState(SessionState state, const std::string& strMessage);
	void Dispatch(const CRequest& req);

private:
	std::mutex m_client_mutex;
	std::unique_ptr<net::CTcpClient> m_client;

private:
	std::atomic_bool m_stopping{ false };
	std::atomic_bool m_connectionAuthenticated{ false };
	std::atomic<SessionState> m_state{ SessionState::Disconnected };

	std::thread m_thread_conn;
	std::thread m_thread_heartbeat;

	std::mutex m_loops_mutex;
	std::condition_variable m_loop_condition;

	mutable std::mutex m_auth_mutex;
	std::optional<CLoginInfo> m_auth;
	std::optional<CLoginInfo> m_login;

	std::mutex m_handlers_mutex;
	std::vector<_TyRequestNotify> m_handlers;

	std::vector<_TyStateHandler> m_state_handlers;
	int m_heartbeat_seconds{ 5 };
	int m_max_reconnect_seconds{ 30 };
};

#endif
