#include "CTradeService.h"
#include "../common/container.h"
#include "../trade/TradeProtocol.h"
#include "../ini/CINIHandler.h"
#include "../network/CNetTools.h"
#include "../request/request.h"
#include <algorithm>
#include <optional>

CTradeService::~CTradeService()
{
	Stop();
}

bool CTradeService::Initialize()
{
	ini::CINIHandler& config = ini::CINIHandler::InstanceRef();
	CLoginInfo info;
	info.m_strToken = config.GetValue(ini::Config::System, "Broker", "token", std::string());
	info.m_host.m_strName = "Broker";
	info.m_host.m_strHost = config.GetValue(ini::Config::System, "Broker", "host", std::string("127.0.0.1"));
	info.m_host.m_nPort = config.GetValue(ini::Config::System, "Broker", "port", 9803);
	m_broker = std::make_unique<CBrokerClient>(info);
	m_broker->SetEventHandler([this](const trade::CCountEvent& event)
	{
		OnEvent(event);
	});
	m_broker->SetCatalogueHandler([this](const CRequest& response)
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		std::erase_if(m_accounts, [&response](const auto& entry)
		{
			return std::none_of(response.GetData().account_list().accounts().begin(), response.GetData().account_list().accounts().end(), [&entry](const request::TradeAccountRecord& row)
			{
				return entry.first == AccountIndex(row);
			});
		});
		for (const auto& row : response.GetData().account_list().accounts())
		{
			CTradeAccountInfo& account = m_accounts[AccountIndex(row)];
			account = AccountInfo(AccountIndex(row));
			account.m_strName = row.name();
			account.m_strBrokerName = row.broker_name();
			trade::CAccountData cached = m_broker->Snapshot(AccountIndex(row));
			account.m_state = request::ACCOUNT_CONNECTED != row.state() ? static_cast<TradeAccountState>(row.state()) : (cached.m_bReady && cached.m_account.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Synchronizing);
		}
	});
	// Missing configuration keeps non-trading XEngine functions available.
	if (!info.Valid())
	{
		m_strLastError = "XBroker token or endpoint is not configured";
		return true;
	}
	return m_broker->Start();
}

void CTradeService::Stop()
{
	if (nullptr != m_broker)
	{
		m_broker->Stop();
	}
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	for (auto& item : m_accounts)
	{
		item.second.m_state = TradeAccountState::Disconnected;
	}
}

void CTradeService::SetEventHandler(_TyEventHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_eventHandler = std::move(handler);
}

bool CTradeService::CanUseAccount(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountIndex);
	return nullptr != m_broker && !container::with(m_disconnectingAccounts, accountIndex) && m_accounts.end() != mIter && TradeAccountState::Connected == mIter->second.m_state;
}

bool CTradeService::HasAccount(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	return m_accounts.contains(accountIndex);
}

bool CTradeService::IsConnected(const _TyAccount& accountIndex) const
{
	return GetAccount(accountIndex).m_bReady;
}

bool CTradeService::AcquireAccount(const _TyAccount& accountIndex)
{
	if (!GetAccount(accountIndex).m_bReady)
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

void CTradeService::ReleaseAccount(const _TyAccount& accountIndex)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accountUsers.find(accountIndex);
	if (m_accountUsers.end() != mIter && 0 == --mIter->second)
	{
		m_accountUsers.erase(mIter);
	}
}

bool CTradeService::HasActiveOrders(const _TyAccount& accountIndex) const
{
	if (!CanUseAccount(accountIndex))
	{
		return false;
	}
	for (const auto& order : m_broker->Snapshot(accountIndex).m_orders)
	{
		if (trade::IsActive(order.m_status))
		{
			return true;
		}
	}
	return false;
}

trade::CResult CTradeService::SendRequest(trade::CReqParam request, CBrokerClient::_TyReplyHandler handler)
{
	if (!GetAccount(request.m_accountIndex).m_bReady)
	{
		return { trade::Error::Unavailable, "Account not synchronized" };
	}
	std::optional<trade::COrder> order;
	if (const trade::CSubmitParam* parameters = std::get_if<trade::CSubmitParam>(&request.m_param); nullptr != parameters)
	{
		order = parameters->m_order;
		order->m_accountIndex = request.m_accountIndex;
	}
	return m_broker->SendRequest(std::move(request), [this, order = std::move(order), handler = std::move(handler)](const trade::CCounterReply& reply)
	{
		if (order.has_value() && ((trade::Error::Rejected == reply.m_result.m_error) || (trade::Error::Unsupported == reply.m_result.m_error) || (trade::Error::Conflict == reply.m_result.m_error) || (trade::Error::NotFound == reply.m_result.m_error)))
		{
			trade::CCountEvent event;
			event.m_kind = trade::EventTy::Order;
			event.m_accountIndex = order->m_accountIndex;
			event.m_order = *order;
			event.m_order.m_status = trade::Status::Rejected;
			OnEvent(event);
		}
		if (nullptr != handler)
		{
			handler(reply);
		}
	});
}

CAccountSnapshot CTradeService::GetAccount(const _TyAccount& accountIndex) const
{
	CAccountSnapshot result;
	if (!CanUseAccount(accountIndex))
	{
		return result;
	}
	trade::CAccountData snapshot = m_broker->Snapshot(accountIndex);
	result.m_bReady = snapshot.m_bReady;
	result.m_version = snapshot.m_version;
	result.m_nTime = snapshot.m_nTime;
	if (!IsAccountValid(snapshot.m_account.m_accountIndex) || !snapshot.m_account.m_bConnected)
	{
		result.m_bReady = false;
		return result;
	}
	result.m_balance = snapshot.m_account.m_nBalance;
	result.m_frozen = snapshot.m_account.m_nFrozen;
	result.m_available = snapshot.m_account.m_bExplicitAvailable ? snapshot.m_account.m_nAvailable : result.m_balance - result.m_frozen;
	result.m_bReady = result.m_bReady && snapshot.m_account.m_bFundsValid;
	return result;
}

CPositionSnapshot CTradeService::GetPosition(const _TyAccount& accountIndex, const CSecurity& security) const
{
	if (CanUseAccount(accountIndex))
	{
		trade::CAccountData state = m_broker->Snapshot(accountIndex);
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		snapshot.m_bReady = GetAccount(accountIndex).m_bReady;
		snapshot.m_version = state.m_version;
		snapshot.m_nTime = state.m_nTime;
		for (const auto& position : state.m_positions)
		{
			if (security.String() == position.m_strSecurity)
			{
				snapshot.m_totalQuantity = position.m_nTotal;
				snapshot.m_availableQuantity = position.m_nAvailable;
				snapshot.m_frozenQuantity = position.m_nFrozen;
				snapshot.m_averageCost = 0 == position.m_nTotal ? 0 : position.m_nCost / position.m_nTotal;
			}
		}
		return snapshot;
	}
	CPositionSnapshot snapshot;
	snapshot.m_security = security;
	return snapshot;
}

const std::string& CTradeService::GetLastError() const
{
	return m_strLastError;
}

bool CTradeService::HandleAccountRequest(const CRequest& req, const std::string& actor)
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
	bool bQueued = m_broker->Forward(std::move(request), [this, original = req, disconnecting, disconnectingAccount](const CRequest& reply)
	{
		if (_TyRequest::TRADE_ACCOUNT_DISCONNECT == original.GetType() && !reply.GetErrorInfo().has_value())
		{
			_TyAccount accountIndex;
			if (ReadAccountIdentity(original, accountIndex))
			{
				std::lock_guard<std::mutex> lock(m_mtx_accounts);
				const auto mIter = m_accounts.find(accountIndex);
				if (m_accounts.end() != mIter)
				{
					mIter->second.m_state = TradeAccountState::Disconnected;
				}
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

void CTradeService::SetPushHandler(_TyPushHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_pushHandler = std::move(handler);
}

void CTradeService::SetAccountReadyHandler(_TyAccountReadyHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_accountReadyHandler = std::move(handler);
}

void CTradeService::OnEvent(const trade::CCountEvent& event)
{
	_TyEventHandler eventHandler;
	_TyPushHandler pushHandler;
	_TyAccountReadyHandler readyHandler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		if (0 != event.m_sequence)
		{
			std::uint64_t& nLast = m_lastEventSequence[event.m_accountIndex];
			if (event.m_sequence <= nLast)
			{
				return;
			}
			nLast = event.m_sequence;
		}
		eventHandler = m_eventHandler;
		pushHandler = m_pushHandler;
		readyHandler = m_accountReadyHandler;
		const auto mIter = m_accounts.find(event.m_accountIndex);
		if (m_accounts.end() == mIter)
		{
			return;
		}
		if (trade::EventTy::Session == event.m_kind)
		{
			mIter->second.m_state = 0 != event.m_account.m_nState ? static_cast<TradeAccountState>(event.m_account.m_nState) : (event.m_account.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Disconnected);
		}
	}
	if (nullptr != eventHandler)
	{
		eventHandler(event);
	}
	if (nullptr != pushHandler)
	{
		CTradeAccountInfo account;
		if (trade::EventTy::Session == event.m_kind)
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			account = m_accounts.at(event.m_accountIndex);
		}
		CRequest push = trade::protocol::Push(event, account);
		pushHandler(push);
	}
	if (trade::EventTy::Session == event.m_kind && event.m_account.m_bConnected && nullptr != readyHandler)
	{
		readyHandler(event.m_accountIndex);
	}
}

bool CTradeService::HandleTradingRequest(const CRequest& req)
{
	if (_TyRequest::TRADE_ORDER_SUBMIT == req.GetType() || _TyRequest::TRADE_ORDER_CANCEL == req.GetType())
	{
		_TyAccount accountIndex;
		if (!ReadAccountIdentity(req, accountIndex) || !GetAccount(accountIndex).m_bReady)
		{
			net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker account is not synchronized");
			return false;
		}
	}
	CRequest request = req;
	bool bQueued = m_broker->Forward(std::move(request), [original = req](const CRequest& reply)
	{
		CRequest response = reply;
		response.SetId(original.GetId());
		net::SendRequest(original.GetConnectionId(), response);
	});
	if (!bQueued)
	{
		net::SendError(req.GetConnectionId(), req, static_cast<int>(trade::Error::Unavailable), "XBroker is unavailable");
	}
	return bQueued;
}

std::vector<trade::COrder> CTradeService::GetOrders(const _TyAccount& accountIndex) const
{
	return nullptr != m_broker && HasAccount(accountIndex) ? m_broker->Snapshot(accountIndex).m_orders : std::vector<trade::COrder>{};
}
