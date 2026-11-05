#include "CAccountService.h"
#include "../common/container.h"
#include "../trade/TradeProtocol.h"
#include "../network/CNetTools.h"
#include <algorithm>
#include <charconv>
#include <set>

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

bool CAccountService::IsConnected(const _TyAccount& account) const
{
	std::shared_lock<std::shared_mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(account);
	return m_accounts.end() != mIter && mIter->second.m_bAtAccountList && !mIter->second.m_bDisconnecting && TradeAccountState::Connected == mIter->second.m_info.m_state;
}

std::vector<_TyAccount> CAccountService::GetAccounts() const
{
	std::shared_lock<std::shared_mutex> lock(m_mtx_accounts);
	std::vector<_TyAccount> accounts;
	accounts.reserve(m_accounts.size());
	for (const auto& entry : m_accounts)
	{
		if (entry.second.m_bAtAccountList)
		{
			accounts.emplace_back(entry.first);
		}
	}
	return accounts;
}

void CAccountService::SetPushHandler(_TyRequestNotify&& handler)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
	m_pushHandler = std::move(handler);
}

void CAccountService::SetAccountStateHandler(_TyAccountStateHandler&& handler)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
	m_accountStateHandler = std::move(handler);
}

void CAccountService::NotifyState(const CAccountInfo& info)
{
	_TyAccountStateHandler stateHandler;
	_TyRequestNotify pushHandler;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_accounts);
		stateHandler = m_accountStateHandler;
		pushHandler = m_pushHandler;
	}
	_TyAccount account = FmtAccount(info);
	if (nullptr != stateHandler)
	{
		stateHandler(account);
	}
	if (nullptr != pushHandler)
	{
		trade::CCounterEvent ev;
		ev.m_kind = trade::EventTy::Session;
		ev.m_account = account;
		ev.m_funds.m_account = account;
		ev.m_funds.m_bConnected = TradeAccountState::Connected == info.m_state;
		ev.m_funds.m_nState = static_cast<int>(info.m_state);
		pushHandler(trade::protocol::Push(ev, info));
	}
}

void CAccountService::OnBrokerEvent(const trade::CCounterEvent& event)
{
	if (trade::EventTy::Session != event.m_kind)
	{
		return;
	}
	CAccountInfo info;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
		const auto mIter = m_accounts.find(event.m_account);
		if (m_accounts.end() == mIter)
		{
			return;
		}
		mIter->second.m_info.m_state = 0 != event.m_funds.m_nState ? static_cast<TradeAccountState>(event.m_funds.m_nState) : (event.m_funds.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Disconnected);
		info = mIter->second.m_info;
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
		std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
		++m_broker_connect_id;
		for (const auto& entry : m_accounts)
		{
			accounts.emplace_back(entry.first);
		}
	}
	for (const auto& v : accounts)
	{
		trade::CCounterEvent event;
		event.m_kind = trade::EventTy::Session;
		event.m_account = v;
		OnBrokerEvent(event);
	}
	FailPending();
}

bool CAccountService::HasAccount(const _TyAccount& account) const
{
	std::shared_lock<std::shared_mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(account);
	return m_accounts.end() != mIter && mIter->second.m_bAtAccountList;
}

bool CAccountService::AcquireAccount(const _TyAccount& account)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(account);
	if ((m_accounts.end() == mIter) || !mIter->second.m_bAtAccountList || mIter->second.m_bDisconnecting || (TradeAccountState::Connected != mIter->second.m_info.m_state))
	{
		return false;
	}
	++mIter->second.m_useCount;
	return true;
}

void CAccountService::ReleaseAccount(const _TyAccount& account)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(account);
	if (m_accounts.end() == mIter || 0 == mIter->second.m_useCount)
	{
		return;
	}
	--mIter->second.m_useCount;
	if (!mIter->second.m_bAtAccountList && !mIter->second.m_bDisconnecting && 0 == mIter->second.m_useCount)
	{
		m_accounts.erase(mIter);
	}
}

void CAccountService::FinishDisconnect(const _TyAccount& account)
{
	std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(account);
	if (m_accounts.end() == mIter)
	{
		return;
	}
	mIter->second.m_bDisconnecting = false;
	if (!mIter->second.m_bAtAccountList && 0 == mIter->second.m_useCount)
	{
		m_accounts.erase(mIter);
	}
}

const std::string& CAccountService::GetLastError() const
{
	return m_strLastError;
}

bool CAccountService::OnRequest(const CRequest& req, const std::string& actor)
{
	_TyRequest t = req.GetType();
	_TyAccount bDisconnectingAccount;
	bool bDisconnecting = (_TyRequest::TRADE_ACCOUNT_DISCONNECT == t) && FmtAccount(req, bDisconnectingAccount);
	if (bDisconnecting)
	{
		bool blocked = false;
		{
			std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
			const auto mIter = m_accounts.find(bDisconnectingAccount);
			blocked = m_accounts.end() != mIter && (mIter->second.m_bDisconnecting || 0 != mIter->second.m_useCount);
			if (!blocked)
			{
				if (m_accounts.end() == mIter)
				{
					CAccountRuntime runtime;
					runtime.m_info = FmtAccountInfo(bDisconnectingAccount);
					runtime.m_bAtAccountList = false;
					runtime.m_bDisconnecting = true;
					m_accounts.emplace(bDisconnectingAccount, std::move(runtime));
				}
				else
				{
					mIter->second.m_bDisconnecting = true;
				}
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
	request.SetExtraData("broker_connect_id", "");
	bool bQueued = SendRequest(std::move(request));
	if (!bQueued)
	{
		if (bDisconnecting)
		{
			FinishDisconnect(bDisconnectingAccount);
		}
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker is unavailable");
	}
	return bQueued;
}

bool CAccountService::OnAccountReply(const CRequest& origin, const CRequest& reply)
{
	_TyAccount account;
	bool bDisconnecting = _TyRequest::TRADE_ACCOUNT_DISCONNECT == origin.GetType() && FmtAccount(origin, account);
	if (bDisconnecting)
	{
		if (!reply.GetErrorInfo().has_value())
		{
			trade::CCounterEvent event;
			event.m_kind = trade::EventTy::Session;
			event.m_account = account;
			OnBrokerEvent(event);
		}
		FinishDisconnect(account);
	}
	CRequest response = reply;
	response.SetId(origin.GetId());

	return net::SendRequest(origin.GetConnectionId(), response);
}

void CAccountService::RefreshAccounts()
{
	if (!m_pSession->IsAuthenticated())
	{
		return;
	}
	std::uint64_t brokerConnectId = 0;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_accounts);
		brokerConnectId = m_broker_connect_id;
	}
	CRequest query;
	query.SetType(_TyRequest::TRADE_ACCOUNT_LIST_QUERY);
	query.SetExtraData("broker_connect_id", std::to_string(brokerConnectId));
	SendRequest(std::move(query));
}

void CAccountService::OnAccountListReply(const CRequest& response, std::uint64_t brokerConnectId)
{
	if (response.GetErrorInfo().has_value() || !response.GetData().has_account_list())
	{
		return;
	}
	std::vector<CAccountInfo> changes;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_accounts);
		if (brokerConnectId != m_broker_connect_id)
		{
			return;
		}
		std::set<_TyAccount> accounts;
		const auto& lst = response.GetData().account_list().accounts();
		for (const auto& row : lst)
		{
			_TyAccount account = FmtAccount(row);
			accounts.emplace(account);
			CAccountInfo info = FmtAccountInfo(account);
			info.m_strName = row.name();
			info.m_strBrokerName = row.broker_name();
			info.m_state = static_cast<TradeAccountState>(row.state());
			const auto mIter = m_accounts.find(account);
			if (m_accounts.end() == mIter || !mIter->second.m_bAtAccountList || info.m_state != mIter->second.m_info.m_state)
			{
				changes.emplace_back(info);
			}
			CAccountRuntime& runtime = m_accounts[account];
			runtime.m_info = std::move(info);
			runtime.m_bAtAccountList = true;
		}
		for (auto& entry : m_accounts)
		{
			if (!container::with(accounts, entry.first) && entry.second.m_bAtAccountList)
			{
				entry.second.m_bAtAccountList = false;
				entry.second.m_info.m_state = TradeAccountState::Disconnected;
				changes.emplace_back(entry.second.m_info);
			}
		}
		std::erase_if(m_accounts, [](const auto& entry)
		{
			return !entry.second.m_bAtAccountList && !entry.second.m_bDisconnecting && 0 == entry.second.m_useCount;
		});
	}
	for (const auto& info : changes)
	{
		NotifyState(info);
	}
}

bool CAccountService::SendRequest(CRequest&& request)
{
	if ((m_bStopping.load()) || !m_pSession->IsAuthenticated())
	{
		return false;
	}
	std::uint64_t nId = CRequest().GetId();
	CRequest origin = request;
	request.SetExtraData("source_request_id", std::to_string(request.GetId()));
	request.SetId(nId);
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_requests);
		if (!m_pSession->IsAuthenticated() || m_bStopping.load())
		{
			return false;
		}
		if (4096 <= m_pending.size())
		{
			return false;
		}
		m_pending.emplace(nId, CPending{ std::move(origin), std::chrono::steady_clock::now() + std::chrono::seconds(_TyRequest::TRADE_ACCOUNT_LOGIN == request.GetType() ? 60 : 15) });
	}
	if (!m_pSession->SendRequest(request))
	{
		// A write failure may follow a partial write. Complete as uncertain, never retry.
		CRequest response;
		response.SetId(nId);
		response.SetType(request.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker write failed; reconcile before retry");
		OnReply(response);
	}
	return true;
}

bool CAccountService::OnReply(const CRequest& req)
{
	if (m_bStopping.load())
	{
		return false;
	}
	if (0 == req.GetId())
	{
		trade::CCounterEvent event;
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
	CRequest origin;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_requests);
		const auto mIter = m_pending.find(req.GetId());
		if (m_pending.end() == mIter || req.GetType() != mIter->second.m_origin.GetType())
		{
			return false;
		}
		origin = std::move(mIter->second.m_origin);
		m_pending.erase(mIter);
	}
	return DispatchReply(origin, req);
}

bool CAccountService::DispatchReply(const CRequest& origin, const CRequest& reply)
{
	_TyRequest t = origin.GetType();
	switch (t)
	{
	case _TyRequest::TRADE_ACCOUNT_LIST_QUERY:
	{
		std::string connectId = origin.GetExtraData("broker_connect_id");
		if (!connectId.empty())
		{
			std::uint64_t brokerConnectId = 0;
			std::from_chars_result result = std::from_chars(connectId.data(), connectId.data() + connectId.size(), brokerConnectId);
			if (std::errc{} != result.ec || connectId.data() + connectId.size() != result.ptr)
			{
				return false;
			}
			OnAccountListReply(reply, brokerConnectId);
			return true;
		}
		return OnAccountReply(origin, reply);
	}
	case _TyRequest::TRADE_ACCOUNT_LOGIN:
	case _TyRequest::TRADE_ACCOUNT_DISCONNECT:
		return OnAccountReply(origin, reply);
	default:
		return false;
	}
}

void CAccountService::FailPending()
{
	std::unordered_map<std::uint64_t, CPending> pending;
	{
		std::unique_lock<std::shared_mutex> lock(m_mtx_requests);
		pending.swap(m_pending);
	}
	for (const auto& item : pending)
	{
		CRequest response;
		response.SetId(item.first);
		response.SetType(item.second.m_origin.GetType());
		response.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker disconnected; reconcile before retry");
		DispatchReply(item.second.m_origin, response);
	}
}

void CAccountService::RefreshPending()
{
	std::vector<std::pair<std::uint64_t, _TyRequest>> expired;
	{
		std::shared_lock<std::shared_mutex> lock(m_mtx_requests);
		for (const auto& item : m_pending)
		{
			if (item.second.m_deadline <= std::chrono::steady_clock::now())
			{
				expired.emplace_back(item.first, item.second.m_origin.GetType());
			}
		}
	}
	for (const auto& item : expired)
	{
		CRequest req;
		req.SetId(item.first);
		req.SetType(item.second);
		req.SetErrorInfo(static_cast<int>(trade::Error::UnknownResult), "XBroker request timed out; reconcile before retry");
		OnReply(req);
	}
}
