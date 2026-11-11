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
#include "../strategy/CAccountService.h"
#include "../strategy/CTradeService.h"
#include "CSession.h"
#include "CHQService.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iterator>
#include <mutex>
#include <sstream>
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
	constexpr std::size_t MinPasswordLength = 6;
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

	m_pTcpServer->RegisterHandler(std::bind_front(&CEngineCenter::OnClientNetEvent, this));

	m_pHQSession->RegisterHandler(std::bind_front(&CEngineCenter::OnHQMarketReply, this));
	m_pHQSession->RegisterStateHandler(std::bind_front(&CEngineCenter::OnHQMarketStateChanged, this));

	m_pBrokerSession->RegisterHandler(std::bind_front(&CEngineCenter::OnBrokerReply, this));
	m_pBrokerSession->RegisterStateHandler(std::bind_front(&CEngineCenter::OnBrokerStateChanged, this));

	m_pAccountService->SetPushHandler(std::bind_front(&CEngineCenter::BroadcastTrade, this));
	m_pTradeService->SetPushHandler(std::bind_front(&CEngineCenter::BroadcastTrade, this));
	return true;
}

bool CEngineCenter::BroadcastTrade(const CRequest& req)
{
	_TyAccount account;
	if (!FmtAccount(req, account))
	{
		return false;
	}
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		return false;
	}
	const db::CTable& owners = pDB->ExecQuery("SELECT u.user FROM table_account a JOIN table_user u ON u.user_id=a.user_id WHERE a.broker_id=" + std::to_string(account.first) + " AND a.account=" + utility::Utf8Literal(account.second) + " AND u.status=1");
	if (owners.m_columns.empty())
	{
		return false;
	}
	std::unordered_set<std::string> users;
	for (const db::_TyRow& row : owners.m_rows)
	{
		users.emplace(db::to_string(row.at(0)));
	}
	std::vector<net::_TyConnectionId> clients;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		clients.reserve(m_authClients.size());
		for (const auto& client : m_authClients)
		{
			if (users.contains(client.second))
			{
				clients.emplace_back(client.first);
			}
		}
	}
	bool bSent = true;
	for (net::_TyConnectionId id : clients)
	{
		bSent = net::SendRequest(id, req) && bSent;
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
		SendResponse(req, InvalidRequest, "密码长度需为 6-128 位");
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

	case _TyRequest::WATCHLIST_QUERY:
	case _TyRequest::WATCHLIST_ADD:
	case _TyRequest::WATCHLIST_REMOVE:
		HandleWatchlistRequest(req, strUser);
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
	case _TyRequest::TRADE_ACCOUNT_ADD:
	case _TyRequest::TRADE_ACCOUNT_REMOVE:
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

bool CEngineCenter::HandleWatchlistRequest(const CRequest& req, const std::string& strUser)
{
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		SendResponse(req, StorageUnavailable, "自选数据库暂不可用");
		return false;
	}
	const db::CTable& userTable = pDB->ExecQuery("SELECT user_id FROM table_user WHERE user=" + utility::Utf8Literal(strUser) + " AND status=1 LIMIT 1");
	if (userTable.m_rows.empty())
	{
		SendResponse(req, StorageUnavailable, "登录用户不存在或数据库不可用");
		return false;
	}
	std::string strUserId = db::to_string(userTable.m_rows.front().at(0));
	if (_TyRequest::WATCHLIST_QUERY != req.GetType())
	{
		std::istringstream stream(req.GetExtraData("securities"));
		std::vector<std::string> securities;
		std::unordered_set<std::string> unique;
		std::string strSecurity;
		while (std::getline(stream, strSecurity))
		{
			std::size_t nDot = strSecurity.find('.');
			bool bValid = (std::string::npos != nDot) && (0 < nDot) && (32 >= strSecurity.size()) &&
				(("SSE" == strSecurity.substr(nDot + 1)) || ("SZSE" == strSecurity.substr(nDot + 1)) ||
				 ("BSE" == strSecurity.substr(nDot + 1)) || ("HKEX" == strSecurity.substr(nDot + 1))) &&
				std::all_of(strSecurity.begin(), strSecurity.begin() + nDot, [](unsigned char ch)
				{
					return 0 != std::isdigit(ch);
				});
			if (!bValid)
			{
				SendResponse(req, InvalidRequest, "无效证券代码：" + strSecurity);
				return false;
			}
			if (unique.emplace(strSecurity).second)
			{
				securities.emplace_back(strSecurity);
			}
			if (500 < securities.size())
			{
				SendResponse(req, InvalidRequest, "单次最多添加或删除 500 只证券");
				return false;
			}
		}
		if (securities.empty())
		{
			SendResponse(req, InvalidRequest, "证券列表不能为空");
			return false;
		}
		std::string strSql;
		if (_TyRequest::WATCHLIST_ADD == req.GetType())
		{
			strSql = "INSERT IGNORE INTO table_user_watchlist(user_id,security) VALUES ";
			for (const std::string& strItem : securities)
			{
				if (' ' != strSql.back())
				{
					strSql += ',';
				}
				strSql += "(" + strUserId + "," + utility::Utf8Literal(strItem) + ")";
			}
		}
		else
		{
			strSql = "DELETE FROM table_user_watchlist WHERE user_id=" + strUserId + " AND security IN (";
			for (const std::string& strItem : securities)
			{
				if ('(' != strSql.back())
				{
					strSql += ',';
				}
				strSql += utility::Utf8Literal(strItem);
			}
			strSql += ')';
		}
		if (0 != pDB->ExecUpdate(strSql))
		{
			SendResponse(req, StorageUnavailable, "保存自选股失败");
			return false;
		}
	}
	const db::CTable& list = pDB->ExecQuery("SELECT security FROM table_user_watchlist WHERE user_id=" + strUserId + " ORDER BY created_at, security");
	if (list.m_columns.empty())
	{
		SendResponse(req, StorageUnavailable, "读取自选股失败");
		return false;
	}
	std::string strList;
	for (const db::_TyRow& row : list.m_rows)
	{
		if (!strList.empty())
		{
			strList += '\n';
		}
		strList += db::to_string(row.at(0));
	}
	CRequest response;
	response.SetId(req.GetId());
	response.SetType(req.GetType());
	response.SetExtraData("securities", strList);
	return net::SendRequest(req.GetConnectionId(), response);
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

int CEngineCenter::OnClientNetEvent(const net::CNetEvent& ev)
{
	if (net::em_event::request == ev.m_event)
	{
		OnRequest(*ev.m_request);
	}
	else if ((net::em_event::disconnected == ev.m_event) || (net::em_event::error == ev.m_event) || (net::em_event::timeout == ev.m_event))
	{
		HandleClientDisconnected(ev.m_connection_id);
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
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB)
	{
		SendResponse(req, StorageUnavailable, "交易账户数据库暂不可用");
		return false;
	}
	const db::CTable& userTable = pDB->ExecQuery("SELECT user_id FROM table_user WHERE user=" + utility::Utf8Literal(strUser) + " AND status=1 LIMIT 1");
	if (userTable.m_rows.empty())
	{
		SendResponse(req, StorageUnavailable, "登录用户不存在或数据库不可用");
		return false;
	}
	std::string strUserId = db::to_string(userTable.m_rows.front().at(0));
	_TyRequest type = req.GetType();
	if (_TyRequest::TRADE_ACCOUNT_LIST_QUERY == type)
	{
		const db::CTable& table = pDB->ExecQuery("SELECT broker_id,account,site,broker_name,account_name FROM table_account WHERE user_id=" + strUserId + " ORDER BY created_at,broker_id,account");
		if (table.m_columns.empty())
		{
			SendResponse(req, StorageUnavailable, "读取交易账户失败");
			return false;
		}
		request::TradeAccountList list;
		for (const db::_TyRow& row : table.m_rows)
		{
			request::TradeAccountRecord* record = list.add_accounts();
			record->set_broker_id(std::stoull(db::to_string(row.at(0))));
			record->set_account(db::to_string(row.at(1)));
			record->set_site(db::to_string(row.at(2)));
			record->set_broker_name(db::to_string(row.at(3)));
			record->set_name(db::to_string(row.at(4)));
			CAccountInfo info = m_pAccountService->GetAccountInfo(FmtAccount(*record));
			record->set_state(static_cast<request::TradingAccountState>(info.m_state));
			record->set_last_error(info.m_strLastError);
		}
		CRequest response;
		response.SetId(req.GetId());
		response.SetType(type);
		response.SetData(list);
		return net::SendRequest(id, response);
	}
	_TyAccount account;
	if (!FmtAccount(req, account) || 64 < account.second.size())
	{
		SendResponse(req, InvalidRequest, "无效交易账户");
		return false;
	}
	std::string strWhere = "user_id=" + strUserId + " AND broker_id=" + std::to_string(account.first) + " AND account=" + utility::Utf8Literal(account.second);
	if (_TyRequest::TRADE_ACCOUNT_ADD == type)
	{
		std::string strSite = req.GetExtraData("site");
		std::string strPassword = req.GetExtraData("password");
		if (strSite.empty() || 64 < strSite.size() || strPassword.empty() || 128 < strPassword.size())
		{
			SendResponse(req, InvalidRequest, "站点或交易密码无效");
			return false;
		}
		std::string strSql = "INSERT INTO table_account(user_id,broker_id,account,site,broker_name,account_name,password) VALUES(" + strUserId + "," + std::to_string(account.first) + "," + utility::Utf8Literal(account.second) + "," + utility::Utf8Literal(strSite) + "," + utility::Utf8Literal(req.GetExtraData("broker_name")) + "," + utility::Utf8Literal(account.second) + "," + utility::Utf8Literal(strPassword) + ") ON DUPLICATE KEY UPDATE site=VALUES(site),broker_name=VALUES(broker_name),password=VALUES(password)";
		if (0 != pDB->ExecUpdate(strSql))
		{
			SendResponse(req, StorageUnavailable, "保存交易账户失败");
			return false;
		}
		SendResponse(req, 0, "交易账户已保存");
		return true;
	}
	const db::CTable& table = pDB->ExecQuery("SELECT site,password FROM table_account WHERE " + strWhere + " LIMIT 1");
	if (table.m_columns.empty())
	{
		SendResponse(req, StorageUnavailable, "读取交易账户失败");
		return false;
	}
	if (table.m_rows.empty())
	{
		SendResponse(req, InvalidRequest, "交易账户未添加到当前用户");
		return false;
	}
	if (_TyRequest::TRADE_ACCOUNT_REMOVE == type)
	{
		if (m_pAccountService->IsConnected(account) || m_pAccountService->IsInUse(account))
		{
			SendResponse(req, InvalidRequest, "请先断开账户并停止关联策略");
			return false;
		}
		if (0 != pDB->ExecUpdate("DELETE FROM table_account WHERE " + strWhere))
		{
			SendResponse(req, StorageUnavailable, "移除交易账户失败");
			return false;
		}
		SendResponse(req, 0, "交易账户已移除");
		return true;
	}
	if (_TyRequest::TRADE_ACCOUNT_LOGIN == type)
	{
		CRequest login = req;
		login.SetExtraData("site", db::to_string(table.m_rows.front().at(0)));
		login.SetExtraData("password", db::to_string(table.m_rows.front().at(1)));
		return m_pAccountService->OnRequest(login, strUser);
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
	if (_TyRequest::SUPPORTED_BROKER_QUERY != req.GetType())
	{
		_TyAccount account;
		if (!FmtAccount(req, account) || !IsOwnedAccount(IsAuthenticated(id).second, account))
		{
			net::SendError(id, req, AuthenticationRequired, "交易账户不属于当前用户");
			return false;
		}
	}
	return m_pTradeService->OnRequest(req);
}

bool CEngineCenter::IsOwnedAccount(const std::string& strUser, const _TyAccount& account) const
{
	db::_TyDBPtr pDB = CDBEngine::InstanceRef().GetDBPtr(db::em_database::mysql);
	if (nullptr == pDB || !IsAccountValid(account))
	{
		return false;
	}
	const db::CTable& table = pDB->ExecQuery("SELECT 1 FROM table_account a JOIN table_user u ON u.user_id=a.user_id WHERE u.user=" + utility::Utf8Literal(strUser) + " AND a.broker_id=" + std::to_string(account.first) + " AND a.account=" + utility::Utf8Literal(account.second) + " AND u.status=1 LIMIT 1");
	return !table.m_rows.empty();
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

int CEngineCenter::HandleClientDisconnected(net::_TyConnectionId id)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		m_authClients.erase(id);
	}
	if (nullptr != m_pHQService)
	{
		m_pHQService->HandleClientDisconnected(id);
	}
	return 1;
}

bool CEngineCenter::OnHQMarketReply(const CRequest& req)
{
	bool bHQHandled = m_pHQService->OnReply(req);
	bool bStrategyHandled = m_pStrategyService->OnReply(req);
	return bHQHandled || bStrategyHandled;
}

bool CEngineCenter::OnBrokerReply(const CRequest& req)
{
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

void CEngineCenter::OnHQMarketStateChanged(SessionState state, const std::string& strReason)
{
	m_pHQService->OnSessionState(state, strReason);
	m_pStrategyService->OnHQMarketState(state, strReason);
}

void CEngineCenter::OnBrokerStateChanged(SessionState state, const std::string& strReason)
{
	m_pAccountService->OnBrokerState(state, strReason);
	m_pTradeService->OnBrokerState(state, strReason);
}
