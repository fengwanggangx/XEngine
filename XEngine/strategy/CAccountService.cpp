#include "CAccountService.h"
#include "../common/container.h"
#include "../trade/TradeProtocol.h"
#include "../network/CNetTools.h"
#include <algorithm>

CAccountService::CAccountService(CBrokerClient* pBroker) : m_pBroker(pBroker)
{
}

CAccountService::~CAccountService()
{
	Stop();
}

bool CAccountService::Initialize()
{
	if (nullptr == m_pBroker)
	{
		m_strLastError = "Broker client is unavailable";
		return false;
	}
	m_bStopping.store(false);
	m_refresh = std::jthread([this](std::stop_token stop)
	{
		while (!stop.stop_requested())
		{
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

void CAccountService::SetPushHandler(_TyPushHandler&& handler)
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
	_TyPushHandler pushHandler;
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

void CAccountService::OnBrokerState(SessionState state)
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
	bool bQueued = m_pBroker->Forward(std::move(request), [this, original = req, disconnecting, disconnectingAccount](const CRequest& reply)
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

		net::SendRequest(original.GetConnectionId(), response);
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
	if (!m_pBroker->IsReady())
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
	m_pBroker->Forward(std::move(query), [this, nGeneration](const CRequest& response)
	{
		OnCatalogue(response, nGeneration);
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
