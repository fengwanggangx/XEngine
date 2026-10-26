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
	m_broker->SetEventHandler([this](const trade::CEvent& event)
	{
		OnEvent(event);
	});
	m_broker->SetCatalogueHandler([this](const CRequest& response)
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		for (const auto& row : response.GetData().account_list().accounts())
		{
			CTradeAccount& account = m_accounts[row.account_id()];
			account.m_info.m_accountId = row.account_id();
			account.m_info.m_strName = row.name();
			account.m_info.m_strNumber = row.number();
			account.m_info.m_type = request::ACCOUNT_SIMULATED == row.account_type() ? TradeAccountType::Simulated : TradeAccountType::Production;
			account.m_info.m_nInitialBalance = row.initial_balance();
			account.m_info.m_strCreatedAt = row.created_at();
			account.m_info.m_strUpdatedAt = row.updated_at();
			account.m_info.m_bDefault = row.is_default();
			account.m_service = m_broker;
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
		item.second.m_info.m_state = TradeAccountState::Disconnected;
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
std::shared_ptr<trade::ITradingGateway> CTradeService::FindService(std::uint64_t accountId) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountId);
	return ((m_accounts.end() == mIter) || (TradeAccountState::Connected != mIter->second.m_info.m_state)) ? nullptr : mIter->second.m_service;
}
bool CTradeService::HasAccount(std::uint64_t accountId) const
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	return m_accounts.contains(accountId);
}
bool CTradeService::IsConnected(std::uint64_t accountId) const
{
	return GetAccount(accountId).m_bReady;
}
bool CTradeService::BeginStrategy(std::uint64_t accountId, _TyStrategyId strategyId)
{
	if (!GetAccount(accountId).m_bReady)
	{
		return false;
	}
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountId);
	if ((m_accounts.end() == mIter) || (TradeAccountState::Connected != mIter->second.m_info.m_state))
	{
		return false;
	}
	mIter->second.m_runningStrategies.emplace(strategyId);
	return true;
}
void CTradeService::EndStrategy(std::uint64_t accountId, _TyStrategyId strategyId)
{
	std::lock_guard<std::mutex> lock(m_mtx_accounts);
	const auto mIter = m_accounts.find(accountId);
	if (m_accounts.end() != mIter)
	{
		mIter->second.m_runningStrategies.erase(strategyId);
	}
}
bool CTradeService::HasActiveOrders(std::uint64_t accountId) const
{
	std::shared_ptr<trade::ITradingGateway> service = FindService(accountId);
	if (nullptr == service)
	{
		return false;
	}
	for (const auto& order : service->Snapshot(accountId).m_orders)
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
	std::shared_ptr<trade::ITradingGateway> service = FindService(intent.m_accountId);
	if ((nullptr == service) || !GetAccount(intent.m_accountId).m_bReady)
	{
		return { false, true, 0, 0, "Account not synchronized" };
	}
	if (OrderType::Limit != intent.m_orderType)
	{
		return { false, false, 0, 0, "UNSUPPORTED: only limit orders" };
	}
	static std::atomic_uint64_t nextId{ static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) };
	trade::CRequest request;
	request.m_operation = trade::Operation::Submit;
	request.m_accountId = intent.m_accountId;
	request.m_order.m_clientId = 0 == intent.m_clientOrderId ? nextId.fetch_add(1) : intent.m_clientOrderId;
	request.m_requestId = request.m_order.m_clientId;
	request.m_order.m_strategyId = intent.m_strategyId;
	request.m_order.m_signalId = intent.m_signalId;
	request.m_order.m_strSecurity = intent.m_security.String();
	request.m_order.m_side = OrderSide::Buy == intent.m_side ? trade::Side::Buy : trade::Side::Sell;
	request.m_order.m_nPrice = intent.m_price;
	request.m_order.m_nQuantity = intent.m_quantity;
	request.m_order.m_nPriceScale = intent.m_nPriceScale;
	trade::Id nClientId = request.m_order.m_clientId;
	trade::COrder order = request.m_order;
	order.m_accountId = request.m_accountId;
	trade::CResult result = service->Request(request, [this, order](const trade::CReply& reply)
	{
		if ((trade::Error::Rejected == reply.m_result.m_error) || (trade::Error::Unsupported == reply.m_result.m_error) || (trade::Error::Conflict == reply.m_result.m_error) || (trade::Error::NotFound == reply.m_result.m_error))
		{
			trade::CEvent event;
			event.m_kind = trade::EventKind::Order;
			event.m_accountId = order.m_accountId;
			event.m_order = order;
			event.m_order.m_status = trade::Status::Rejected;
			OnEvent(event);
		}
	});
	return { result.Ok(), !result.Ok(), nClientId, 0, result.Ok() ? "Request queued; await order event" : result.m_strMessage };
}
bool CTradeService::Cancel(_TyStrategyId strategyId, _TyOrderId orderId)
{
	std::uint64_t accountId = 0;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		const auto mIter = m_orderAccounts.find(orderId);
		if (m_orderAccounts.end() == mIter)
		{
			return false;
		}
		accountId = mIter->second;
	}
	std::shared_ptr<trade::ITradingGateway> service = FindService(accountId);
	if (nullptr == service)
	{
		return false;
	}
	trade::CSnapshot snapshot = service->Snapshot(accountId);
	if (!std::any_of(snapshot.m_orders.begin(), snapshot.m_orders.end(), [strategyId, orderId](const trade::COrder& row)
	{
		return orderId == row.m_orderId && strategyId == row.m_strategyId;
	}))
	{
		return false;
	}
	trade::CRequest request;
	request.m_operation = trade::Operation::Cancel;
	request.m_accountId = accountId;
	request.m_orderId = orderId;
	return service->Request(request, {}).Ok();
}
CAccountSnapshot CTradeService::GetAccount() const
{
	return GetAccount(1);
}
CAccountSnapshot CTradeService::GetAccount(std::uint64_t accountId) const
{
	std::shared_ptr<trade::ITradingGateway> service = FindService(accountId);
	CAccountSnapshot result;
	if (nullptr == service)
	{
		return result;
	}
	trade::CSnapshot snapshot = service->Snapshot(accountId, false);
	result.m_bReady = snapshot.m_bReady && (5000 >= std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - snapshot.m_nTime);
	result.m_version = snapshot.m_version;
	result.m_nTime = snapshot.m_nTime;
	if (snapshot.m_accounts.empty() || !snapshot.m_accounts[0].m_bConnected)
	{
		result.m_bReady = false;
		return result;
	}
	result.m_balance = snapshot.m_accounts[0].m_nBalance;
	result.m_frozen = snapshot.m_accounts[0].m_nFrozen;
	result.m_available = result.m_balance - result.m_frozen;
	return result;
}
CPositionSnapshot CTradeService::GetPosition(const CSecurity& security) const
{
	return GetPosition(1, security);
}
CPositionSnapshot CTradeService::GetPosition(std::uint64_t accountId, const CSecurity& security) const
{
	std::shared_ptr<trade::ITradingGateway> service = FindService(accountId);
	if (nullptr != service)
	{
		trade::CSnapshot state = service->Snapshot(accountId, false);
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		snapshot.m_bReady = GetAccount(accountId).m_bReady;
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
		std::uint64_t nAccountId = 0;
		if (ReadId(req.GetExtraData("account_id"), nAccountId))
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			auto iter = m_accounts.find(nAccountId);
			if (m_accounts.end() != iter && !iter->second.m_runningStrategies.empty())
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
		CRequest response = reply;
		response.SetId(original.GetId());
		if (response.GetData().has_account_list())
		{
			request::TradeAccountList list = response.GetData().account_list();
			std::unordered_map<std::uint64_t, std::uint64_t> counts;
			db::_TyDBPtr database = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
			if (nullptr != database)
			{
				const db::CTable& rows = database->ExecQuery("SELECT account_id,COUNT(*) FROM table_strategy WHERE account_provider='broker' GROUP BY account_id");
				for (const auto& row : rows.m_rows)
				{
					std::uint64_t nId = 0;
					std::uint64_t nCount = 0;
					if (2 == row.size() && ReadId(db::to_string(row[0]), nId) && utility::to_number(db::to_string(row[1]), nCount))
					{
						counts.emplace(nId, nCount);
					}
				}
			}
			database.reset();
			for (auto& row : *list.mutable_accounts())
			{
				row.set_strategy_count(static_cast<std::uint32_t>(counts[row.account_id()]));
			}
			response.SetData(list);
		}
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
void CTradeService::OnEvent(const trade::CEvent& event)
{
	_TyOrderEventHandler orderHandler;
	_TyTradeEventHandler tradeHandler;
	_TyPushHandler pushHandler;
	_TyAccountReadyHandler readyHandler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		if (0 != event.m_sequence)
		{
			std::uint64_t& nLast = m_lastEventSequence[event.m_accountId];
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
		const auto mIter = m_accounts.find(event.m_accountId);
		if (m_accounts.end() == mIter)
		{
			return;
		}
		if (trade::EventKind::Session == event.m_kind)
		{
			mIter->second.m_info.m_state = event.m_account.m_bConnected ? TradeAccountState::Connected : TradeAccountState::Disconnected;
		}
		if (trade::EventKind::Order == event.m_kind)
		{
			if (0 != event.m_order.m_orderId)
			{
				if (trade::IsActive(event.m_order.m_status))
				{
					m_orderAccounts[event.m_order.m_orderId] = event.m_accountId;
				}
				else
				{
					m_orderAccounts.erase(event.m_order.m_orderId);
				}
			}
		}
	}
	if (trade::EventKind::Order == event.m_kind && nullptr != orderHandler)
	{
		COrderEvent order = trade::protocol::OrderEvent(event);
		orderHandler(order);
	}
	if (trade::EventKind::Trade == event.m_kind && nullptr != tradeHandler)
	{
		CTradeEvent trade = trade::protocol::TradeEvent(event);
		tradeHandler(trade);
	}
	if (nullptr != pushHandler)
	{
		CTradeAccountInfo account;
		if (trade::EventKind::Session == event.m_kind)
		{
			std::lock_guard<std::mutex> lock(m_mtx_accounts);
			account = m_accounts.at(event.m_accountId).m_info;
		}
		CRequest push = trade::protocol::Push(event, account);
		pushHandler(push);
	}
	if (trade::EventKind::Session == event.m_kind && event.m_account.m_bConnected && nullptr != readyHandler)
	{
		readyHandler(event.m_accountId);
	}
}
bool CTradeService::HandleTradingRequest(const CRequest& req)
{
	if (_TyRequest::TRADE_ORDER_SUBMIT == req.GetType() || _TyRequest::TRADE_ORDER_CANCEL == req.GetType())
	{
		std::uint64_t nAccountId = 0;
		if (!ReadId(req.GetExtraData("account_id"), nAccountId) || !GetAccount(nAccountId).m_bReady)
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

std::vector<trade::COrder> CTradeService::GetOrders(std::uint64_t accountId) const
{
	std::shared_ptr<trade::ITradingGateway> service;
	{
		std::lock_guard<std::mutex> lock(m_mtx_accounts);
		const auto mIter = m_accounts.find(accountId);
		if (m_accounts.end() != mIter)
		{
			service = mIter->second.m_service;
		}
	}
	return nullptr == service ? std::vector<trade::COrder>{} : service->Snapshot(accountId).m_orders;
}
