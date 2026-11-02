#include "CSession.h"
#include "../request/RequestCenter.h"
#include "../network/CTcpClient.h"
#include "../network/common_net.h"
#include <algorithm>
#include <iostream>
#include <utility>

CSession::CSession(const CLoginInfo& info)
{
	m_auth = info;
}

CSession::~CSession()
{
	Stop();
}

bool CSession::Start()
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_auth);
		bool bCredentialsValid = (m_auth.has_value() && m_auth->Valid()) || (m_login.has_value() && m_login->Valid());
		if (!bCredentialsValid)
		{
			return false;
		}
	}
	if (m_thread_conn.joinable() || m_thread_heartbeat.joinable())
	{
		return true;
	}
	m_bStopping.store(false);
	m_thread_conn = std::thread(&CSession::ConnectionLoop, this);
	m_thread_heartbeat = std::thread(&CSession::MaintenanceLoop, this);
	return true;
}

void CSession::Stop()
{
	if (!m_thread_conn.joinable() && !m_thread_heartbeat.joinable())
	{
		return;
	}
	m_bStopping.store(true);
	NotifyState(SessionState::Stopping, "upstream session is stopping");
	m_cv_loops.notify_all();
	{
		std::lock_guard<std::mutex> lock(m_mtx_client);
		if (nullptr != m_client)
		{
			m_client->ShutDown();
		}
	}
	if (m_thread_heartbeat.joinable())
	{
		m_thread_heartbeat.join();
	}
	if (m_thread_conn.joinable())
	{
		m_thread_conn.join();
	}
	{
		decltype(m_client) pClient;
		{
			std::lock_guard<std::mutex> lock(m_mtx_client);
			pClient = std::move(m_client);
		}
		if (nullptr != pClient)
		{
			pClient->Release();
		}
	}
	NotifyState(SessionState::Disconnected, "upstream session stopped");
}

bool CSession::SendRequest(const CRequest& request)
{
	_TyRequest t = request.GetType();
	bool bAuthRequest = (_TyRequest::AUTH_LOGIN == t) || (_TyRequest::AUTH_REGISTER == t);
	if (!bAuthRequest && !IsAuthenticated())
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mtx_client);
	return (nullptr != m_client) && m_client->SendRequest(request);
}

void CSession::RegisterHandler(_TyRequestNotify&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_handlers.emplace_back(std::move(handler));
}

void CSession::SetStateHandler(_TyStateHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_stateHandlers.clear();
	if (nullptr != handler)
	{
		m_stateHandlers.emplace_back(std::move(handler));
	}
}

void CSession::RegisterStateHandler(_TyStateHandler&& handler)
{
	if (nullptr == handler)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_stateHandlers.emplace_back(std::move(handler));
}

SessionState CSession::GetState() const
{
	return m_state.load();
}

bool CSession::IsConnected() const
{
	SessionState state = m_state.load();
	return (SessionState::Connected == state) || (SessionState::Authenticating == state) || (SessionState::Ready == state);
}

bool CSession::IsAuthenticated() const
{
	return SessionState::Ready == m_state.load();
}

void CSession::ConnectionLoop()
{
	int nReConnectSeconds = 1;
	while (!m_bStopping.load())
	{
		NotifyState(1 == nReConnectSeconds ? SessionState::Connecting : SessionState::Reconnecting, 1 == nReConnectSeconds ? "Connecting to upstream" : "Reconnecting to upstream in " + std::to_string(nReConnectSeconds) + " seconds");
		std::optional<CLoginInfo> info;
		{
			std::lock_guard<std::mutex> lock(m_mtx_auth);
			info = m_auth.has_value() ? m_auth : m_login;
		}
		if (!info.has_value() || !info->Valid())
		{
			NotifyState(SessionState::Disconnected, "upstream login information is invalid");
			break;
		}
		decltype(m_client) pClient = std::make_unique<net::CTcpClient>(info->m_host.m_strHost, static_cast<int>(info->m_host.m_nPort), info->m_bOrdered);
		pClient->RegisterHandler(std::bind_front(&CSession::OnNetEvent, this));
		int nRet = pClient->Initialize();
		if (0 == nRet)
		{
			{
				std::lock_guard<std::mutex> lock(m_mtx_client);
				m_client = std::move(pClient);
			}
			m_client->Start(true);
		}
		else
		{
			std::cerr << "upstream connection initialization failed: " << nRet << '\n';
		}
		bool bAuthed = IsAuthenticated();
		decltype(m_client) pClosedClient;
		{
			std::lock_guard<std::mutex> lock(m_mtx_client);
			pClosedClient = std::move(m_client);
		}
		if (nullptr != pClosedClient)
		{
			pClosedClient->Release();
		}
		if (m_bStopping.load())
		{
			break;
		}
		NotifyState(SessionState::Disconnected, "upstream connection closed");
		std::unique_lock<std::mutex> lock(m_mtx_loops);
		m_cv_loops.wait_for(lock, std::chrono::seconds(nReConnectSeconds), [this]()
		{
			return m_bStopping.load();
		});
		nReConnectSeconds = bAuthed ? 1 : (std::min)(m_nMaxReconnectSeconds, nReConnectSeconds * 2);
	}
}

void CSession::MaintenanceLoop()
{
	std::chrono::steady_clock::time_point nTmNextHearbeat = std::chrono::steady_clock::now();
	while (!m_bStopping.load())
	{
		std::unique_lock<std::mutex> lock(m_mtx_loops);
		m_cv_loops.wait_for(lock, std::chrono::milliseconds(250), [this]()
		{
			return m_bStopping.load();
		});
		lock.unlock();
		if (m_bStopping.load())
		{
			break;
		}
		std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		if (IsAuthenticated() && (nTmNextHearbeat <= now))
		{
			SendRequest(request::HeartBeat());
			nTmNextHearbeat = now + std::chrono::seconds(m_nHeartbeatSeconds);
		}
	}
}
int CSession::OnNetEvent(const net::CNetEvent& ev)
{
	if (net::em_event::connected == ev.m_event)
	{
		NotifyState(SessionState::Connected, "Connected to upstream");
		SendAuthentication();
		return 1;
	}
	if ((net::em_event::request == ev.m_event) && (nullptr != ev.m_request))
	{
		HandleResponse(*ev.m_request);
		return 1;
	}
	if ((net::em_event::disconnected == ev.m_event) || (net::em_event::error == ev.m_event) || (net::em_event::timeout == ev.m_event))
	{
		NotifyState(SessionState::Disconnected, "upstream connection lost");
		return 1;
	}
	return 0;
}

bool CSession::HandleAuthRequest(const CRequest& req)
{
	if (req.GetErrorInfo().has_value())
	{
		bool bRetryWithCredentials = false;
		{
			std::lock_guard<std::mutex> lock(m_mtx_auth);
			if (m_login.has_value() && !m_login->m_strToken.empty())
			{
				m_login->m_strToken.clear();
				bRetryWithCredentials = !m_login->m_strUser.empty() && !m_login->m_strPassword.empty();
			}
		}
		if (bRetryWithCredentials)
		{
			SendAuthentication();
			return false;
		}
		std::optional<std::pair<int, std::string>> errorInfo = req.GetErrorInfo();
		NotifyState(SessionState::Connected, "upstream authentication failed: " + (errorInfo.has_value() ? errorInfo->second : req.GetExtraData("reason")));
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(m_mtx_auth);
		if (m_auth.has_value())
		{
			m_auth->m_strToken = req.GetExtraData("token");
			m_login = std::move(m_auth);
			m_auth.reset();
		}
		else if (m_login.has_value() && !req.GetExtraData("token").empty())
		{
			m_login->m_strToken = req.GetExtraData("token");
		}
	}
	m_state.store(SessionState::Ready);
	{
		std::lock_guard<std::mutex> lock(m_mtx_client);
		if (nullptr != m_client)
		{
			m_client->SetReadTimeout(20);
		}
	}
	NotifyState(SessionState::Ready, "upstream session ready");
	return true;
}

void CSession::HandleResponse(const CRequest& req)
{
	if (request::IsAuthRequest(req))
	{
		HandleAuthRequest(req);
	}
	else
	{
		Dispatch(req);
	}
}

bool CSession::SendAuthentication()
{
	std::optional<CLoginInfo> info;
	{
		std::lock_guard<std::mutex> lock(m_mtx_auth);
		info = m_auth.has_value() ? m_auth : m_login;
	}
	if (!info.has_value() || !info->Valid())
	{
		NotifyState(SessionState::Connected, "upstream credentials are invalid");
		return false;
	}
	NotifyState(SessionState::Authenticating, "Authenticating with upstream");
	if (!info->m_strToken.empty())
	{
		return SendRequest(request::Auth(info->m_strToken));
	}
	return SendRequest(request::Auth(info->m_strUser, info->m_strPassword));
}

void CSession::NotifyState(SessionState state, const std::string& strMessage)
{
	m_state.store(state);
	std::vector<_TyStateHandler> handlers;
	{
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		handlers = m_stateHandlers;
	}
	for (const auto& handler : handlers)
	{
		if (nullptr != handler)
		{
			handler(state, strMessage);
		}
	}
}

void CSession::Dispatch(const CRequest& req)
{
	std::vector<_TyRequestNotify> handlers;
	{
		std::lock_guard<std::mutex> lock(m_mtx_handlers);
		handlers = m_handlers;
	}
	for (const auto& handler : handlers)
	{
		if (nullptr != handler)
		{
			handler(req);
		}
	}
}

void CSession::ClearHandlers()
{
	std::lock_guard<std::mutex> lock(m_mtx_handlers);
	m_handlers.clear();
	m_stateHandlers.clear();
}
