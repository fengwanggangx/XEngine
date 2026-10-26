#ifndef HQMARKET_REQUEST_REQUEST_H
#define HQMARKET_REQUEST_REQUEST_H

#include "../network/common_net.h"
#include "MarketTypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace request
{
	enum class types
	{
		UNKNOWN = 0,
		AUTH_LOGIN = 1,
		AUTH_REGISTER = 3,
		HEARTBEAT = 6,
		TRADE_ACCOUNT_LOGIN = 8,
		TRADE_ACCOUNT_QUERY = 9,
		TRADE_ACCOUNT_ADD = 10,
		TRADE_ACCOUNT_MODIFY = 11,
		TRADE_ACCOUNT_CONNECT = 12,
		TRADE_ACCOUNT_DISCONNECT = 13,
		TRADE_ACCOUNT_INFO_QUERY = 14,
		TRADE_ACCOUNT_LOGS_QUERY = 15,
		STRATEGY_ADD = 20,
		STRATEGY_MODIFY = 21,
		STRATEGY_QUERY = 22,
		STRATEGY_DELETE = 23,
		STRATEGY_RUNTIME_QUERY = 24,
		STRATEGY_START = 25,
		STRATEGY_PAUSE = 26,
		STRATEGY_STOP = 27,
		MARKET_SUBSCRIBE = 40,
		MARKET_UNSUBSCRIBE = 41,
		MARKET_QUOTE_QUERY = 42,
		MARKET_BARS_QUERY = 43,
		MARKET_SECURITIES_QUERY = 44,
		MARKET_SECTORS_QUERY = 45,
		MARKET_SECTOR_CONSTITUENTS_QUERY = 46,
		MARKET_QUOTE_PUSH = 47,
		MARKET_DEPTH_PUSH = 48,
		TRADE_ORDER_SUBMIT = 49,
		TRADE_ORDER_CANCEL = 50,
		TRADE_POSITION_QUERY = 51,
		TRADE_ORDER_QUERY = 52,
		TRADE_TRADE_QUERY = 53,
		TRADE_ACCOUNT_STATE_PUSH = 54,
		TRADE_ORDER_PUSH = 55,
		TRADE_TRADE_PUSH = 56,
		TRADE_ACCOUNT_INFO_PUSH = 57,
		TRADE_POSITION_PUSH = 58,
		TRADE_SYNC_QUERY = 59,
		TRADE_CAPABILITIES_QUERY = 60,
	};

	class RequestData;
	class TradeOrderList;
	class TradeList;
	class TradePositionList;
	class TradeCounterReply;
	class StrategyInfo;
	class StrategyList;
	class TradeAccountRecord;
	class TradeAccountList;
	class TradeAccountInfo;
	class StrategyRuntimeList;
	class TradeAccountLogList;
} // namespace request

namespace google::protobuf
{
	class Arena;
}

using _TyRequest = request::types;
using _TyRequestId = std::uint64_t;
using _TyReqData = request::RequestData;
using _TyStrategyInfo = request::StrategyInfo;
using _TyStrategyList = request::StrategyList;
class CRequest
{
	public:
	CRequest();
	~CRequest();
	CRequest(const CRequest& arg);
	CRequest& operator=(const CRequest& arg);
	CRequest(CRequest&& arg) noexcept;
	CRequest& operator=(CRequest&& arg) noexcept;

	public:
	_TyRequestId GetId() const;
	void SetId(_TyRequestId id);

	_TyRequest GetType() const;
	void SetType(_TyRequest t);
	bool IsMarket() const;
	bool IsStrategy() const;
	std::string GetTypeName() const;

	std::string GetExtraData(const std::string& strKey) const;
	std::unordered_map<std::string, std::string> GetExtraData() const;
	void SetExtraData(const std::string& strKey, const std::string& strValue);
	using _TyExtraRows = std::vector<std::unordered_map<std::string, std::string>>;
	void SetExtraData(const std::string& strKey, const std::vector<std::uint64_t>& values);
	void SetExtraData(const std::string& strKey, const _TyExtraRows& rows);
	bool GetExtraData(const std::string& strKey, std::vector<std::uint64_t>& values) const;
	bool GetExtraData(const std::string& strKey, _TyExtraRows& rows) const;

	void SetData(const _TySubscriptionAck& value);

	void SetData(const _TyQuoteData& value);
	void SetData(const _TyDepthData& value);
	void SetData(const _TyQueryResponse& value);
	void SetData(const _TySecurityList& value);
	void SetData(const _TySectorList& value);
	void SetData(const _TySectorConstituents& value);
	void SetData(const _TyStrategyInfo& value);
	void SetData(const _TyStrategyList& value);
	void SetData(const request::TradeAccountRecord& value);
	void SetData(const request::TradeAccountList& value);
	void SetData(const request::TradeAccountInfo& value);
	void SetData(const request::StrategyRuntimeList& value);
	void SetData(const request::TradeAccountLogList& value);
	void SetData(const request::TradeOrderList& value);
	void SetData(const request::TradeList& value);
	void SetData(const request::TradePositionList& value);
	void SetData(const request::TradeCounterReply& value);
	const _TyReqData& GetData() const;

	void SetConnectionId(net::_TyConnectionId id);
	net::_TyConnectionId GetConnectionId() const;

	bool Serialize(std::string* pOutput) const;
	bool Deserialize(const std::string& strData);

	std::optional<std::pair<int, std::string>> GetErrorInfo() const;
	void SetErrorInfo(int nCode, const std::string& strMessage);

	private:
	std::unique_ptr<google::protobuf::Arena> m_arena;
	_TyReqData* m_data{ nullptr };
	net::_TyConnectionId m_connection_id{ -1 };
};

#endif
