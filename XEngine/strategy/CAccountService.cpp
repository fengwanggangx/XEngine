#include "CAccountService.h"
#include "../common/container.h"
#include "../trade/TradeProtocol.h"
#include "../network/CNetTools.h"
#include <algorithm>

CAccountService::CAccountService(CSession* pSession) : m_pSession(pSession)
{
}

CAccountService::~CAccountService()
{
	Stop();
}

bool CAccountService::Initialize()
{
	if (nullptr == m_pSession)
	{
		m_strLastError = "Broker session is unavailable";
		return false;
	}
	m_bStopping.store(false);
	m_pSession->RegisterHandler(std::bind_front(&CAccountService::OnRequestReply, this));
	m_pSession->RegisterStateHandler(std::bind_front(&CAccountService::OnBrokerState, this));
	m_refresh = std::jthread([this](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
			RefreshPending();
			RefreshAccounts();
			std::unique_lock<std::mutex> lock(m_mtx_wait);
			m_wake.wait_for(lock, std::chrono::seconds(1), [&stop]()
			{
				return stop.stop_requested();
			});
		}
	});
	return true;
}

void CAccountService::Stop()
{
	if (m_bStopping.exchange(true))
	{
		return;
	}
	m_refresh.request_stop();
	m_wake.notify_all();
	if (m_refresh.joinable())
	{
		m_refresh.join();
	}
	OnBrokerState(SessionState::Stopping);
}

bool CAccountService::IsConnected(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountIndex);
	return !container::with(m_disconnectingAccounts, accountIndex) && m_accounts.end() != mIter && TradeAccountState::Connected == mIter->second.m_state;
}

std::vector<_TyAccount> CAccountService::GetAccounts() const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	std::vector<_TyAccount> accounts;
	accounts.reserve(m_accounts.size());
	for (const auto& entry : m_accounts)
	{
		accounts.emplace_back(entry.first);
	}
	return accounts;
}

void CAccountService::SetPushHandler(_TyRequestNotify&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_pushHandler = std::move(handler);
}

void CAccountService::SetAccountStateHandler(_TyAccountStateHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_accountStateHandler = std::move(handler);
}

void CAccountService::NotifyState(const CAccountInfo& info)
{
	_TyAccountStateHandler stateHandler;
	_TyRequestNotify pushHandler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		stateHandler = m_accountStateHandler;
		pushHandler = m_pushHandler;
	}
	_TyAccount accountIndex = AccountIndex(info);
	if (nullptr != stateHandler)
	{
		stateHandler(accountIndex);
	}
	if (nullptr != pushHandler)
	{
		trade::CCountEvent event;
		event.m_kind = trade::EventTy::Session;
		event.m_accountIndex = accountIndex;
		event.m_account.m_accountIndex = accountIndex;
		event.m_account.m_bConnected = TradeAccountState::Connected == info.m_state;
		event.m_account.m_nState = static_cast<int>(info.m_state);
		pushHandler(trade::protocol::Push(event, info));
	}
}

void CAccountService::OnBrokerEvent(const trade::CCountEvent& event)
{
	if (trade::EventTy::Session != event.m_kind)
	{
		return;
	}
	CAccountInfo info;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		const auto mIter = m_accounts.find(event.m_accountIndex);
		if (m_accounts.end() == mIter)
		{
			return;
		}
		mIter->second.m_state = 0 != event.m_account.m_nState ? static_cast<TradeAccountState>(event.m_account.m_nState) : (event.m_account.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Disconnected);
		info = mIter->second;
	}
	NotifyState(info);
}

void CAccountService::OnBrokerState(SessionState state, const std::string&)
{
	if (SessionState::Ready == state)
	{
		return;
	}
	std::vector<_TyAccount> accounts;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		++m_generation;
		for (const auto& entry : m_accounts)
		{
			accounts.emplace_back(entry.first);
		}
	}
	for (const auto& account : accounts)
	{
		trade::CCountEvent event;
		event.m_kind = trade::EventTy::Session;
		event.m_accountIndex = account;
		OnBrokerEvent(event);
	}
	FailPending();
}

bool CAccountService::HasAccount(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	return m_accounts.contains(accountIndex);
}

bool CAccountService::AcquireAccount(const _TyAccount& accountIndex)
{
	if (!IsConnected(accountIndex))
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountIndex);
	if (container::with(m_disconnectingAccounts, accountIndex) || (m_accounts.end() == mIter) || (TradeAccountState::Connected != mIter->second.m_state))
	{
		return false;
	}
	++m_accountUsers[accountIndex];
	return true;
}

void CAccountService::ReleaseAccount(const _TyAccount& accountIndex)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accountUsers.find(accountIndex);
	if (m_accountUsers.end() != mIter && 0 == --mIter->second)
	{
		m_accountUsers.erase(mIter);
	}
}

const std::string& CAccountService::GetLastError() const
{
	return m_strLastError;
}

bool CAccountService::HandleAccountRequest(const CRequest& req, const std::string& actor)
{
	_TyAccount disconnectingAccount;
	bool disconnecting = _TyRequest::TRADE_ACCOUNT_DISCONNECT == req.GetType() && ReadAccountIdentity(req, disconnectingAccount);
	if (disconnecting)
	{
		bool blocked = false;
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			const auto mIter = m_accountUsers.find(disconnectingAccount);
			blocked = container::with(m_disconnectingAccounts, disconnectingAccount) || (m_accountUsers.end() != mIter && 0 != mIter->second);
			if (!blocked)
			{
				m_disconnectingAccounts.emplace(disconnectingAccount);
			}
		}
		if (blocked)
		{
			net::SendError(req.GetConnectionId(), req, 1202, "Account is disconnecting or in use");
			return false;
		}
	}
	CRequest request = req;
	request.SetExtraData("actor", actor);
	bool bQueued = SendRequest(std::move(request), [this, original = req, disconnecting, disconnectingAccount](const CRequest& reply)
	{
		if (_TyRequest::TRADE_ACCOUNT_DISCONNECT == original.GetType() && !reply.GetErrorInfo().has_value())
		{
			_TyAccount accountIndex;
			if (ReadAccountIdentity(original, accountIndex))
			{
				trade::CCountEvent event;
				event.m_kind = trade::EventTy::Session;
				event.m_accountIndex = accountIndex;
				OnBrokerEvent(event);
			}
		}
		if (disconnecting)
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			m_disconnectingAccounts.erase(disconnectingAccount);
		}
		CRequest response = reply;
		response.SetId(original.GetId());

		return net::SendRequest(original.GetConnectionId(), response);
	});
	if (!bQueued)
	{
		if (disconnecting)
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			m_disconnectingAccounts.erase(disconnectingAccount);
		}
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker is unavailable");
	}
	return bQueued;
}

void CAccountService::RefreshAccounts()
{
	if (!m_pSession->IsAuthenticated())
	{
		return;
	}
	std::uint64_t nGeneration = 0;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		nGeneration = m_generation;
	}
	CRequest query;
	query.SetType(_TyRequest::TRADE_ACCOUNT_LIST_QUERY);
	SendRequest(std::move(query), [this, nGeneration](const CRequest& response)
	{
		OnCatalogue(response, nGeneration);
		return true;
	});
}

void CAccountService::OnCatalogue(const CRequest& response, std::uint64_t nGeneration)
{
	if (response.GetErrorInfo().has_value() || !response.GetData().has_account_list())
	{
		return;
	}
	std::vector<CAccountInfo> changes;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		if (nGeneration != m_generation)
		{
			return;
		}
		std::set<_TyAccount> accounts;
		for (const auto& row : response.GetData().account_list().accounts())
		{
			_TyAccount accountIndex = AccountIndex(row);
			accounts.emplace(accountIndex);
			CAccountInfo info = AccountInfo(accountIndex);
			info.m_strName = row.name();
			info.m_strBrokerName = row.broker_name();
			info.m_state = static_cast<TradeAccountState>(row.state());
			const auto mIter = m_accounts.find(accountIndex);
			if (m_accounts.end() == mIter || info.m_state != mIter->second.m_state)
			{
				changes.emplace_back(info);
			}
			m_accounts[accountIndex] = std::move(info);
		}
		std::erase_if(m_accounts, [&accounts, &changes](const auto& entry)
		{
			if (container::with(accounts, entry.first))
			{
				return false;
			}
			changes.emplace_back(entry.second);
			changes.back().m_state = TradeAccountState::Disconnected;
			return true;
		});
	}
	for (const auto& info : changes)
	{
		NotifyState(info);
	}
}

bool CAccountService::SendRequest(CRequest&& request, _TyRequestNotify&& handler)
{
	if ((m_bStopping.load()) || !m_pSession->IsAuthenticated())
	{
		return false;
	}
	std::uint64_t nId = CRequest().GetId();
	request.SetExtraData("source_request_id", std::to_string(request.GetId()));
	request.SetId(nId);
	{
		std::lock_guard<std::mutex> lock(m_mtx_requests);
		if (!m_pSession->IsAuthenticated() || m_bStopping.load())
		{
			return false;
		}
		if (4096 <= m_pending.size())
		{
			return false;
		}
		m_pending.emplace(nId, CPending{ std::move(handler), std::chrono::steady_clock::now() + std::chrono::seconds(_TyRequest::TRADE_ACCOUNT_LOGIN == request.GetType() ? 60 : 15), request.GetType() });
	}
	if (!m_pSession->SendRequest(request))
	{
		// A write failure may follow a partial write. Complete as uncertain, never retry.
		CRequest response;
		response.SetId(nId);
		response.SetType(request.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker write failed; reconcile before retry");
		OnRequestReply(response);
	}
	return true;
}

bool CAccountService::OnRequestReply(const CRequest& req)
{
	if (m_bStopping.load())
	{
		return false;
	}
	if (0 == req.GetId())
	{
		trade::CCountEvent event;
		if (!trade::protocol::ReadPush(req, event))
		{
			return false;
		}
		if (trade::EventTy::Session != event.m_kind)
		{
			return false;
		}
		OnBrokerEvent(event);
		return true;
	}
	_TyRequestNotify handler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_requests);
		const auto mIter = m_pending.find(req.GetId());
		if (m_pending.end() == mIter || req.GetType() != mIter->second.m_type)
		{
			return false;
		}
		handler = std::move(mIter->second.m_handler);
		m_pending.erase(mIter);
	}
	return (nullptr != handler) && handler(req);
}

void CAccountService::FailPending()
{
	std::unordered_map<std::uint64_t, CPending> pending;
	{
		std::lock_guard<std::mutex> lock(m_mtx_requests);
		pending.swap(m_pending);
	}
	for (const auto& item : pending)
	{
		CRequest response;
		response.SetId(item.first);
		response.SetType(item.second.m_type);
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker disconnected; reconcile before retry");
		if (nullptr != item.second.m_handler)
		{
			item.second.m_handler(response);
		}
	}
}

void CAccountService::RefreshPending()
{
	std::vector<std::pair<std::uint64_t, _TyRequest>> expired;
	{
		std::lock_guard<std::mutex> lock(m_mtx_requests);
		for (const auto& item : m_pending)
		{
			if (item.second.m_deadline <= std::chrono::steady_clock::now())
			{
				expired.emplace_back(item.first, item.second.m_type);
			}
		}
	}
	for (const auto& item : expired)
	{
		CRequest req;
		req.SetId(item.first);
		req.SetType(item.second);
		req.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker request timed out; reconcile before retry");
		OnRequestReply(req);
	}
}
