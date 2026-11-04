#include "CEngineCenter.h"

#include "../common/utility.h"
#include "../database/CDBEngine.h"
#include "../database/IDataBase.h"
#include "../network/CNetTools.h"
#include "../network/CTcpServer.h"
#include "../network/common_net.h"
#include "../request/RequestCenter.h"
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../request/v1/market.pb.h"
#include "../strategy/CStrategyService.h"
#include "../strategy/CTradeService.h"
#include "CSession.h"
#include "CHQService.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	constexpr int InvalidRequest = 1001;
	constexpr int InvalidCredentials = 1002;
	constexpr int UserExists = 1003;
	constexpr int StorageUnavailable = 1004;
	constexpr std::size_t MinUserLength = 3;
	constexpr std::size_t MaxUserLength = 64;
	constexpr std::size_t MinPasswordLength = 8;
	constexpr std::size_t MaxPasswordLength = 128;
	constexpr int AuthenticationRequired = 1005;
	constexpr int UpstreamUnavailable = 1007;
	constexpr std::chrono::hours TokenLifetime{ 24 };
} // namespace

bool IsUserValid(const std::string& strUser)
{
	if ((MinUserLength > strUser.size()) || (MaxUserLength < strUser.size()))
	{
		return false;
	}
	for (unsigned char c : strUser)
	{
		if ((0 == std::isalnum(c)) && ('_' != c) && ('-' != c) && ('.' != c) && ('@' != c))
		{
			return false;
		}
	}
	return true;
}

bool IsPasswordValid(const std::string& strPassword)
{
	return (MinPasswordLength <= strPassword.size()) && (MaxPasswordLength >= strPassword.size());
}

CEngineCenter::CEngineCenter(net::CTcpServer* pTcpServer, CSession* pHQSession, CSession* pBrokerSession, CHQService* pHQService, CStrategyService* pStrategyService, CTradeService* pTradeService, CAccountService* pAccountService) : m_pTcpServer(pTcpServer), m_pHQSession(pHQSession), m_pBrokerSession(pBrokerSession), m_pHQService(pHQService), m_pStrategyService(pStrategyService), m_pTradeService(pTradeService), m_pAccountService(pAccountService)
{
}

bool CEngineCenter::Initialize()
{
	if ((nullptr == m_pTcpServer) || (nullptr == m_pHQSession) || (nullptr == m_pBrokerSession) || (nullptr == m_pHQService) || (nullptr == m_pStrategyService) || (nullptr == m_pTradeService) || (nullptr == m_pAccountService))
	{
		return false;
	}
	m_pAccountService->SetPushHandler(std::bind_front(&CEngineCenter::BroadcastTrading, this));
	m_pTradeService->SetPushHandler(std::bind_front(&CEngineCenter::BroadcastTrading, this));
	m_pTcpServer->RegisterHandler(std::bind_front(&CEngineCenter::OnNetEvent, this));
	m_pHQSession->RegisterHandler(std::bind_front(&CEngineCenter::OnReply, this, SessionSource::HQMarket));
	m_pBrokerSession->RegisterHandler(std::bind_front(&CEngineCenter::OnReply, this, SessionSource::Broker));
	m_pHQSession->RegisterStateHandler(std::bind_front(&CEngineCenter::OnSessionState, this, SessionSource::HQMarket));
	m_pBrokerSession->RegisterStateHandler(std::bind_front(&CEngineCenter::OnSessionState, this, SessionSource::Broker));
	return true;
}

bool CEngineCenter::BroadcastTrading(const CRequest& push)
{
	std::vector<net::_TyConnectionId> clients;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		clients.reserve(m_authClients.size());
		for (const auto& client : m_authClients)
		{
			clients.emplace_back(client.first);
		}
	}
	bool bSent = true;
	for (net::_TyConnectionId id : clients)
	{
		bSent = net::SendRequest(id, push) && bSent;
	}
	return bSent;
}

void CEngineCenter::SendResponse(const CRequest& req, int nErrorCode, const std::string& strMessage) const
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	if (0 != nErrorCode)
	{
		net::SetError(response, nErrorCode, strMessage);
	}
	else
	{

		response.SetErrorInfo(0, "");
	}
	net::SendRequest(req.GetConnectionId(), response);
}

bool CEngineCenter::Login(const CRequest& req, std::string& strToken)
{
	std::string strUser = req.GetExtraData("user");
	std::string strPassword = req.GetExtraData("password");
	if (!IsUserValid(strUser) || !IsPasswordValid(strPassword))
	{
		SendResponse(req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == db)
	{
		SendResponse(req, StorageUnavailable, "用户数据库暂不可用");
		return false;
	}

	std::string strSql = "SELECT user_id, user FROM table_user WHERE user=" + utility::Utf8Literal(strUser) + " AND password_hash=UNHEX(SHA2(CONCAT(password_salt,UNHEX('" + utility::ToHex(strPassword) + "')),256)) AND status=1 LIMIT 1";
	const db::CTable& table = db->ExecQuery(strSql);
	if (table.m_rows.empty())
	{
		SendResponse(req, InvalidCredentials, "账号或密码错误");
		return false;
	}

	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());

	response.SetExtraData("user_id", db::to_string(table.m_rows.front().at(0)));
	response.SetExtraData("user", db::to_string(table.m_rows.front().at(1)));
	strToken = utility::MakeSaltHex();
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_authClients.insert_or_assign(req.GetConnectionId(), strUser);
		m_tokenUsers.insert_or_assign(strToken, strUser);
		m_clientTokens.insert_or_assign(strToken, std::chrono::steady_clock::now() + TokenLifetime);
	}
	response.SetExtraData("token", strToken);
	net::SendRequest(req.GetConnectionId(), response);
	return true;
}

bool CEngineCenter::HandleRegisterAuth(net::_TyConnectionId, const CRequest& req)
{
	std::string strUser = req.GetExtraData("user");
	std::string strPassword = req.GetExtraData("password");
	if (!IsUserValid(strUser))
	{
		SendResponse(req, InvalidRequest, "账号需为 3-64 位字母、数字或 _-.@");
		return false;
	}
	if (!IsPasswordValid(strPassword))
	{
		SendResponse(req, InvalidRequest, "密码长度需为 8-128 位");
		return false;
	}

	db::_TyDBPtr db = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == db)
	{
		SendResponse(req, StorageUnavailable, "用户数据库暂不可用");
		return false;
	}

	std::string strUserLiteral = utility::Utf8Literal(strUser);
	const db::CTable& table = db->ExecQuery("SELECT user_id FROM table_user WHERE user=" + strUserLiteral + " LIMIT 1");
	if (!table.m_rows.empty())
	{
		SendResponse(req, UserExists, "账号已存在");
		return false;
	}

	std::string saltHex = utility::MakeSaltHex();
	std::string strSQL = "INSERT INTO table_user(user,password_hash,password_salt,status) VALUES(" + strUserLiteral + ",UNHEX(SHA2(CONCAT(UNHEX('" + saltHex + "'),UNHEX('" + utility::ToHex(strPassword) + "')),256)),UNHEX('" + saltHex + "'),1)";
	if (0 != db->ExecUpdate(strSQL))
	{
		SendResponse(req, UserExists, "账号已存在或注册失败");
		return false;
	}

	SendResponse(req, 0, "注册成功");
	return true;
}

bool CEngineCenter::HandleReAuth(const CRequest& req)
{
	std::string strToken = req.GetExtraData("token");
	bool bAccepted = false;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		const auto mIter = m_clientTokens.find(strToken);
		bAccepted = (m_clientTokens.end() != mIter) && (std::chrono::steady_clock::now() < mIter->second);
		if ((m_clientTokens.end() != mIter) && !bAccepted)
		{
			m_clientTokens.erase(mIter);
			m_tokenUsers.erase(strToken);
		}
		if (bAccepted)
		{
			m_authClients.insert_or_assign(req.GetConnectionId(), m_tokenUsers.at(strToken));
		}
	}
	if (bAccepted)
	{
		SendResponse(req, 0, "认证成功");
		return true;
	}
	SendResponse(req, InvalidCredentials, "登录状态已失效");
	return false;
}

std::pair<bool, std::string> CEngineCenter::IsAuthenticated(net::_TyConnectionId id) const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	const auto mIter = m_authClients.find(id);
	if (m_authClients.end() == mIter)
	{
		return { false, std::string() };
	}
	return { true, mIter->second };
}

bool CEngineCenter::HandleAuth(net::_TyConnectionId id, const CRequest& req)
{
	// 断线重连
	std::string strToken = req.GetExtraData("token");
	if (!strToken.empty())
	{
		return HandleReAuth(req);
	}

	// 登录认证
	if (request::IsUserAuthRequest(req))
	{
		Login(req, strToken);
		return true;
	}
	net::SendError(req.GetConnectionId(), req, InvalidRequest, "unsupported authentication request");
	return false;
}

void CEngineCenter::OnRequest(const CRequest& req)
{
	net::_TyConnectionId id = req.GetConnectionId();
	if (request::IsUserAuthRequest(req))
	{
		HandleAuth(id, req);
		return;
	}
	if (request::IsUserRegisterRequest(req))
	{
		HandleRegisterAuth(id, req);
		return;
	}
	std::pair<bool, std::string> authentication = IsAuthenticated(id);
	bool bAuthenticated = authentication.first;
	const std::string& strUser = authentication.second;
	if (!bAuthenticated)
	{
		net::SendError(id, req, 1002, "authentication required");
		return;
	}

	_TyRequest type = req.GetType();
	switch (type)
	{
	case _TyRequest::TRADE_ORDER_SUBMIT:
	case _TyRequest::TRADE_ORDER_CANCEL:
	case _TyRequest::TRADE_POSITION_QUERY:
	case _TyRequest::TRADE_ORDER_QUERY:
	case _TyRequest::TRADE_DEAL_QUERY:
	case _TyRequest::SUPPORTED_BROKER_QUERY:
	case _TyRequest::TRADE_FUNDS_QUERY:
		HandleTradeRequest(id, req);
		return;

	case _TyRequest::HEARTBEAT:
		HandleHeartbeat(id, req);
		return;

	case _TyRequest::MARKET_SUBSCRIBE:
	case _TyRequest::MARKET_UNSUBSCRIBE:
		m_pHQService->OnRequest(req);
		return;

	case _TyRequest::MARKET_QUOTE_QUERY:
	case _TyRequest::MARKET_BARS_QUERY:
	case _TyRequest::MARKET_SECURITIES_QUERY:
	case _TyRequest::MARKET_SECTORS_QUERY:
	case _TyRequest::MARKET_SECTOR_CONSTITUENTS_QUERY:
		m_pHQService->OnRequest(req);
		return;

	case _TyRequest::TRADE_ACCOUNT_LOGIN:
	case _TyRequest::TRADE_ACCOUNT_LIST_QUERY:
	case _TyRequest::TRADE_ACCOUNT_DISCONNECT:
		HandleTradeAccountRequest(id, req, strUser);
		return;

	case _TyRequest::STRATEGY_ADD:
	case _TyRequest::STRATEGY_MODIFY:
	case _TyRequest::STRATEGY_QUERY:
	case _TyRequest::STRATEGY_DELETE:
	case _TyRequest::STRATEGY_RUNTIME_QUERY:
	case _TyRequest::STRATEGY_START:
	case _TyRequest::STRATEGY_PAUSE:
	case _TyRequest::STRATEGY_STOP:
		HandleStrategyRequest(id, req);
		return;

	default:
		net::SendError(id, req, 1006, "unknown request type");
		return;
	}
}

bool CEngineCenter::HandleHeartbeat(net::_TyConnectionId id, const CRequest& req)
{
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(_TyRequest::HEARTBEAT);
	response.SetExtraData("client_time_ms", req.GetExtraData("client_time_ms"));

	return net::SendRequest(id, response);
}

bool CEngineCenter::HandleStrategyRequest(net::_TyConnectionId id, const CRequest& req)
{
	if ((!req.IsStrategy()) || (nullptr == m_pStrategyService))
	{
		net::SendError(id, req, InvalidRequest, "invalid strategy request");
		return false;
	}
	return m_pStrategyService->OnRequest(req);
}

int CEngineCenter::OnNetEvent(const net::CNetEvent& ev)
{
	if (net::em_event::request == ev.m_event)
	{
		OnRequest(*ev.m_request);
	}
	else if ((net::em_event::disconnected == ev.m_event) || (net::em_event::error == ev.m_event) || (net::em_event::timeout == ev.m_event))
	{
		HandleDisconnected(ev.m_connection_id);
	}
	return 1;
}

bool CEngineCenter::HandleTradeAccountRequest(net::_TyConnectionId id, const CRequest& req, const std::string& strUser)
{
	if (nullptr == m_pAccountService)
	{
		net::SendError(id, req, UpstreamUnavailable, "Account service is unavailable");
		return false;
	}
	return m_pAccountService->OnRequest(req, strUser);
}

bool CEngineCenter::HandleTradeRequest(net::_TyConnectionId id, const CRequest& req)
{
	if (!IsAuthenticated(id).first || (nullptr == m_pTradeService))
	{
		net::SendError(id, req, AuthenticationRequired, "authentication required");
		return false;
	}
	return m_pTradeService->OnRequest(req);
}

CEngineCenter::~CEngineCenter()
{
	if (nullptr != m_pAccountService)
	{
		m_pAccountService->SetPushHandler({});
	}
	if (nullptr != m_pTradeService)
	{
		m_pTradeService->SetPushHandler({});
	}
}

int CEngineCenter::HandleDisconnected(net::_TyConnectionId id)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_authClients.erase(id);
	}
	if (nullptr != m_pHQService)
	{
		m_pHQService->HandleDisconnected(id);
	}
	return 1;
}

bool CEngineCenter::OnReply(SessionSource source, const CRequest& req)
{
	if (SessionSource::HQMarket == source)
	{
		bool bHQHandled = m_pHQService->OnReply(req);
		bool bStrategyHandled = m_pStrategyService->OnReply(req);
		return bHQHandled || bStrategyHandled;
	}
	if (0 == req.GetId())
	{
		bool bAccountHandled = false;
		if (_TyRequest::TRADE_ACCOUNT_STATE_PUSH == req.GetType())
		{
			bAccountHandled = m_pAccountService->OnReply(req);
		}
		bool bTradeHandled = m_pTradeService->OnReply(req);
		return bAccountHandled || bTradeHandled;
	}
	switch (req.GetType())
	{
	case _TyRequest::TRADE_ACCOUNT_LOGIN:
	case _TyRequest::TRADE_ACCOUNT_DISCONNECT:
	case _TyRequest::TRADE_ACCOUNT_LIST_QUERY:
		return m_pAccountService->OnReply(req);
	default:
		return m_pTradeService->OnReply(req);
	}
}

void CEngineCenter::OnSessionState(SessionSource source, SessionState state, const std::string& strReason)
{
	if (SessionSource::HQMarket == source)
	{
		m_pHQService->OnSessionState(state, strReason);
		m_pStrategyService->OnHQMarketState(state, strReason);
		return;
	}
	m_pAccountService->OnBrokerState(state, strReason);
	m_pTradeService->OnBrokerState(state, strReason);
}
