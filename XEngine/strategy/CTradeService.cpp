#include "CTradeService.h"
#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../trade/TradeProtocol.h"
#include "../ini/CINIHandler.h"
#include "../network/CNetTools.h"
#include "../request/RequestCenter.h"
#include "../request/request.h"
#include "../system/CSession.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <limits>
#include <map>
namespace
{
	using trade::protocol::WriteOrder;
	using trade::protocol::WritePosition;
	using trade::protocol::WriteTrade;
	bool ReadId(const std::string& strText, std::uint64_t& nValue)
	{
		std::from_chars_result result = std::from_chars(strText.data(), strText.data() + strText.size(), nValue);
		return !strText.empty() && (std::errc() == result.ec) && (strText.data() + strText.size() == result.ptr) && (0 != nValue);
	}

} // namespace

CTradeService::~CTradeService()
{
	Stop();
}

bool CTradeService::Initialize(CSession&)
{
	ini::CINIHandler& config = ini::CINIHandler::InstanceRef();
	CLoginInfo info;
	info.m_strToken = config.GetValue(ini::Config::System, "Broker", "token", std::string());
	info.m_host.m_strName = "Broker";
	info.m_host.m_strHost = config.GetValue(ini::Config::System, "Broker", "host", std::string("127.0.0.1"));
	info.m_host.m_nPort = config.GetValue(ini::Config::System, "Broker", "port", 9803);
	m_broker = std::make_shared<CBrokerClient>(info);
	m_broker->SetEventHandler([this](const trade::CCountEvent& event)
	{
		OnEvent(event);
	});
	m_broker->SetCatalogueHandler([this](const CRequest& response)
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		std::erase_if(m_accounts, [&response](const auto& entry)
		{
			bool removed = std::none_of(response.GetData().account_list().accounts().begin(), response.GetData().account_list().accounts().end(), [&entry](const request::TradeAccountRecord& row)
			{
				return entry.first == AccountIndex(row);
			});
			if (removed && nullptr != entry.second.m_session)
			{
				entry.second.m_session->Close();
			}
			return removed;
		});
		for (const auto& row : response.GetData().account_list().accounts())
		{
			CTradeAccount& account = m_accounts[AccountIndex(row)];
			account.m_info = AccountInfo(AccountIndex(row));
			account.m_info.m_strName = row.name();
			account.m_info.m_strBrokerName = row.broker_name();
			if (nullptr == account.m_session)
			{
				account.m_session = m_broker->CreateSession(AccountIndex(row));
			}
			trade::CAccountData cached = m_broker->Snapshot(AccountIndex(row));
			account.m_info.m_state = request::ACCOUNT_CONNECTED != row.state() ? static_cast<TradeAccountState>(row.state()) : (cached.m_bReady && cached.m_account.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Synchronizing);
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
	std::vector<std::shared_ptr<trade::ITradeSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		for (auto& item : m_accounts)
		{
			item.second.m_info.m_state = TradeAccountState::Disconnected;
			if (nullptr != item.second.m_session)
			{
				sessions.emplace_back(std::move(item.second.m_session));
			}
		}
	}
	for (const auto& session : sessions)
	{
		session->Close();
	}
}

void CTradeService::SetOrderEventHandler(_TyOrderEventHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_orderHandler = std::move(handler);
}

void CTradeService::SetTradeEventHandler(_TyTradeEventHandler&& handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	m_tradeHandler = std::move(handler);
}

std::shared_ptr<trade::ITradeSession> CTradeService::FindService(const _TyAccount& accountIndex) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountIndex);
	return ((m_accounts.end() == mIter) || (TradeAccountState::Connected != mIter->second.m_info.m_state)) ? nullptr : mIter->second.m_session;
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

bool CTradeService::BeginStrategy(const _TyAccount& accountIndex, _TyStrategyId strategyId)
{
	if (!GetAccount(accountIndex).m_bReady)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountIndex);
	if ((m_accounts.end() == mIter) || (TradeAccountState::Connected != mIter->second.m_info.m_state))
	{
		return false;
	}
	m_runningStrategies[accountIndex].emplace(strategyId);
	return true;
}

void CTradeService::EndStrategy(const _TyAccount& accountIndex, _TyStrategyId strategyId)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountIndex);
	if (m_accounts.end() != mIter)
	{
		m_runningStrategies[accountIndex].erase(strategyId);
	}
}

bool CTradeService::HasActiveOrders(const _TyAccount& accountIndex) const
{
	std::shared_ptr<trade::ITradeSession> service = FindService(accountIndex);
	if (nullptr == service)
	{
		return false;
	}
	for (const auto& order : service->Snapshot().m_orders)
	{
		if (trade::IsActive(order.m_status))
		{
			return true;
		}
	}
	return false;
}

COrderSubmitResult CTradeService::Submit(const COrderIntent& intent)
{
	std::shared_ptr<trade::ITradeSession> service = FindService(intent.m_accountIndex);
	if ((nullptr == service) || !GetAccount(intent.m_accountIndex).m_bReady)
	{
		return { false, true, 0, 0, "Account not synchronized" };
	}
	if (OrderType::Limit != intent.m_orderType)
	{
		return { false, false, 0, 0, "UNSUPPORTED: only limit orders" };
	}
	static std::atomic_uint64_t nextId{ static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) };
	trade::CReqParam request;
	trade::CSubmitParam& params = request.m_param.emplace<trade::CSubmitParam>();
	request.m_accountIndex = intent.m_accountIndex;
	params.m_order.m_clientId = 0 == intent.m_clientOrderId ? nextId.fetch_add(1) : intent.m_clientOrderId;
	request.m_requestId = params.m_order.m_clientId;
	params.m_order.m_strategyId = intent.m_strategyId;
	params.m_order.m_signalId = intent.m_signalId;
	params.m_order.m_strSecurity = intent.m_security.String();
	params.m_order.m_side = OrderSide::Buy == intent.m_side ? trade::Side::Buy : trade::Side::Sell;
	params.m_order.m_nPrice = intent.m_price;
	params.m_order.m_nQuantity = intent.m_quantity;
	params.m_order.m_nPriceScale = intent.m_nPriceScale;
	std::uint64_t nClientId = params.m_order.m_clientId;
	trade::COrder order = params.m_order;
	order.m_accountIndex = request.m_accountIndex;
	trade::CResult result = service->SendRequest(request, [this, order](const trade::CCounterReply& reply)
	{
		if ((trade::Error::Rejected == reply.m_result.m_error) || (trade::Error::Unsupported == reply.m_result.m_error) || (trade::Error::Conflict == reply.m_result.m_error) || (trade::Error::NotFound == reply.m_result.m_error))
		{
			trade::CCountEvent event;
			event.m_kind = trade::EventTy::Order;
			event.m_accountIndex = order.m_accountIndex;
			event.m_order = order;
			event.m_order.m_status = trade::Status::Rejected;
			OnEvent(event);
		}
	});
	return { result.Ok(), !result.Ok(), nClientId, 0, result.Ok() ? "Request queued; await order event" : result.m_strMessage };
}

bool CTradeService::Cancel(_TyStrategyId strategyId, _TyOrderId orderId)
{
	_TyAccount accountIndex;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		const auto mIter = m_orderAccounts.find({ strategyId, orderId });
		if (m_orderAccounts.end() == mIter)
		{
			return false;
		}
		accountIndex = mIter->second;
	}
	std::shared_ptr<trade::ITradeSession> service = FindService(accountIndex);
	if (nullptr == service)
	{
		return false;
	}
	trade::CAccountData snapshot = service->Snapshot();
	if (!std::any_of(snapshot.m_orders.begin(), snapshot.m_orders.end(), [strategyId, orderId](const trade::COrder& row)
	{
		return orderId == row.m_orderId && strategyId == row.m_strategyId;
	}))
	{
		return false;
	}
	trade::CReqParam request;
	request.m_param = trade::CCancelParam{ orderId };
	request.m_accountIndex = accountIndex;
	return service->SendRequest(request, {}).Ok();
}

CAccountSnapshot CTradeService::GetAccount() const
{
	return {};
}

CAccountSnapshot CTradeService::GetAccount(const _TyAccount& accountIndex) const
{
	std::shared_ptr<trade::ITradeSession> service = FindService(accountIndex);
	CAccountSnapshot result;
	if (nullptr == service)
	{
		return result;
	}
	trade::CAccountData snapshot = service->Snapshot();
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

CPositionSnapshot CTradeService::GetPosition(const CSecurity& security) const
{
	CPositionSnapshot result;
	result.m_security = security;
	return result;
}

CPositionSnapshot CTradeService::GetPosition(const _TyAccount& accountIndex, const CSecurity& security) const
{
	std::shared_ptr<trade::ITradeSession> service = FindService(accountIndex);
	if (nullptr != service)
	{
		trade::CAccountData state = service->Snapshot();
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
	if (_TyRequest::TRADE_ACCOUNT_DISCONNECT == req.GetType())
	{
		_TyAccount accountIndex;
		if (ReadAccountIdentity(req, accountIndex))
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			auto iter = m_accounts.find(accountIndex);
			if (m_accounts.end() != iter && !m_runningStrategies[accountIndex].empty())
			{
				net::SendError(req.GetConnectionId(), req, 1202, "Stop strategies before disconnecting account");
				return false;
			}
		}
	}
	CRequest request = req;
	request.SetExtraData("actor", actor);
	bool bQueued = m_broker->Forward(std::move(request), [this, original = req](const CRequest& reply)
	{
		if (_TyRequest::TRADE_ACCOUNT_DISCONNECT == original.GetType() && !reply.GetErrorInfo().has_value())
		{
			_TyAccount accountIndex;
			if (ReadAccountIdentity(original, accountIndex))
			{
				std::shared_ptr<trade::ITradeSession> session;
				{
					std::lock_guard<std::mutex> lock(m_mtx_accounts);
					const auto mIter = m_accounts.find(accountIndex);
					if (m_accounts.end() != mIter)
					{
						session = std::move(mIter->second.m_session);
						mIter->second.m_info.m_state = TradeAccountState::Disconnected;
					}
				}
				if (nullptr != session)
				{
					session->Close();
				}
			}
		}
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
	_TyOrderEventHandler orderHandler;
	_TyTradeEventHandler tradeHandler;
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
		orderHandler = m_orderHandler;
		tradeHandler = m_tradeHandler;
		pushHandler = m_pushHandler;
		readyHandler = m_accountReadyHandler;
		const auto mIter = m_accounts.find(event.m_accountIndex);
		if (m_accounts.end() == mIter)
		{
			return;
		}
		if (trade::EventTy::Session == event.m_kind)
		{
			mIter->second.m_info.m_state = 0 != event.m_account.m_nState ? static_cast<TradeAccountState>(event.m_account.m_nState) : (event.m_account.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Disconnected);
		}
		if (trade::EventTy::Order == event.m_kind)
		{
			if (0 != event.m_order.m_orderId)
			{
				if (trade::IsActive(event.m_order.m_status))
				{
					m_orderAccounts[{ event.m_order.m_strategyId, event.m_order.m_orderId }] = event.m_accountIndex;
				}
				else
				{
					m_orderAccounts.erase({ event.m_order.m_strategyId, event.m_order.m_orderId });
				}
			}
		}
	}
	if (trade::EventTy::Order == event.m_kind && nullptr != orderHandler)
	{
		COrderEvent order = trade::protocol::OrderEvent(event);
		orderHandler(order);
	}
	if (trade::EventTy::Deal == event.m_kind && nullptr != tradeHandler)
	{
		CTradeEvent trade = trade::protocol::TradeEvent(event);
		tradeHandler(trade);
	}
	if (nullptr != pushHandler)
	{
		CTradeAccountInfo account;
		if (trade::EventTy::Session == event.m_kind)
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			account = m_accounts.at(event.m_accountIndex).m_info;
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
	std::shared_ptr<trade::ITradeSession> service;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		const auto mIter = m_accounts.find(accountIndex);
		if (m_accounts.end() != mIter)
		{
			service = mIter->second.m_session;
		}
	}
	return nullptr == service ? std::vector<trade::COrder>{} : service->Snapshot().m_orders;
}
