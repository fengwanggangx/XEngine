#include "request.h"

#include "request.pb.h"
#include "v1/market.pb.h"

#include <atomic>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

CRequest::CRequest() : m_arena(std::make_unique<google::protobuf::Arena>())
{
	m_data = google::protobuf::Arena::CreateMessage<_TyReqData>(m_arena.get());

	static std::atomic<_TyRequestId> s_id{ 1 };
	SetId(s_id.fetch_add(1, std::memory_order_relaxed));
}

CRequest::~CRequest() = default;

CRequest::CRequest(CRequest&& arg) noexcept = default;

CRequest& CRequest::operator=(CRequest&& arg) noexcept = default;

CRequest::CRequest(const CRequest& arg) : CRequest()
{
	*this = arg;
}

CRequest& CRequest::operator=(const CRequest& arg)
{
	if (this != &arg)
	{
		m_data->CopyFrom(*arg.m_data);
		m_connection_id = arg.m_connection_id;
	}
	return *this;
}

_TyRequestId CRequest::GetId() const
{
	return m_data->id();
}

void CRequest::SetId(_TyRequestId id)
{
	m_data->set_id(id);
}

_TyRequest CRequest::GetType() const
{
	return static_cast<_TyRequest>(m_data->type());
}

void CRequest::SetType(_TyRequest t)
{
	m_data->set_type(static_cast<request::RequestType>(t));
}

std::string CRequest::GetExtraData(const std::string& strKey) const
{
	const auto mIter = m_data->extra().find(strKey);
	return m_data->extra().end() == mIter ? std::string() : mIter->second;
}

std::unordered_map<std::string, std::string> CRequest::GetExtraData() const
{
	return { m_data->extra().begin(), m_data->extra().end() };
}

void CRequest::SetExtraData(const std::string& strKey, const std::string& strValue)
{
	(*m_data->mutable_extra())[strKey] = strValue;
}

void CRequest::SetData(const _TySubscriptionAck& value)
{
	m_data->mutable_subscription_ack()->CopyFrom(value);
}

void CRequest::SetData(const _TyQuoteData& value)
{
	m_data->mutable_quote()->CopyFrom(value);
}

void CRequest::SetData(const _TyDepthData& value)
{
	m_data->mutable_depth()->CopyFrom(value);
}

void CRequest::SetData(const _TyQueryResponse& value)
{
	m_data->mutable_query_response()->CopyFrom(value);
}

void CRequest::SetData(const _TySecurityList& value)
{
	m_data->mutable_security_list()->CopyFrom(value);
}

void CRequest::SetData(const _TySectorList& value)
{
	m_data->mutable_sector_list()->CopyFrom(value);
}

void CRequest::SetData(const _TySectorConstituents& value)
{
	m_data->mutable_sector_constituents()->CopyFrom(value);
}

void CRequest::SetData(const _TyStrategyInfo& value)
{
	m_data->mutable_strategy()->CopyFrom(value);
}

void CRequest::SetData(const _TyStrategyList& value)
{
	m_data->mutable_strategy_list()->CopyFrom(value);
}

const _TyReqData& CRequest::GetData() const
{
	return *m_data;
}

void CRequest::SetConnectionId(net::_TyConnectionId id)
{
	m_connection_id = id;
}

net::_TyConnectionId CRequest::GetConnectionId() const
{
	return m_connection_id;
}

bool CRequest::Serialize(std::string* pOutput) const
{
	return (nullptr != pOutput) && m_data->SerializeToString(pOutput);
}

bool CRequest::Deserialize(const std::string& strData)
{
	return m_data->ParseFromString(strData);
}

void CRequest::SetErrorInfo(int nCode, const std::string& strMessage)
{
	m_data->set_error_code(nCode);
	m_data->set_error_message(strMessage);
}

std::optional<std::pair<int, std::string>> CRequest::GetErrorInfo() const
{
	if (0 == m_data->error_code())
	{
		return std::nullopt;
	}
	return std::pair<int, std::string>{ m_data->error_code(), m_data->error_message() };
}

void CRequest::SetData(const request::TradeAccountRecord& value)
{
	m_data->mutable_account()->CopyFrom(value);
}

void CRequest::SetData(const request::TradeAccountList& value)
{
	m_data->mutable_account_list()->CopyFrom(value);
}

void CRequest::SetData(const request::TradeAccountInfo& value)
{
	m_data->mutable_account_info()->CopyFrom(value);
}

void CRequest::SetData(const request::TradeAccountLogList& value)
{
	m_data->mutable_account_logs()->CopyFrom(value);
}

bool CRequest::IsMarket() const
{
	return (_TyRequest::MARKET_SUBSCRIBE <= GetType()) && (_TyRequest::MARKET_DEPTH_PUSH >= GetType());
}
bool CRequest::IsStrategy() const
{
	return (_TyRequest::STRATEGY_ADD <= GetType()) && (_TyRequest::STRATEGY_STOP >= GetType());
}
std::string CRequest::GetTypeName() const
{
	return request::RequestType_Name(m_data->type());
}

void CRequest::SetExtraData(const std::string& strKey, const std::vector<std::uint64_t>& values)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartArray();
	for (std::uint64_t nValue : values)
	{
		writer.Uint64(nValue);
	}
	writer.EndArray();
	SetExtraData(strKey, std::string(buffer.GetString(), buffer.GetSize()));
}
void CRequest::SetExtraData(const std::string& strKey, const _TyExtraRows& rows)
{
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartArray();
	for (const auto& row : rows)
	{
		writer.StartObject();
		for (const auto& item : row)
		{
			writer.Key(item.first.data(), static_cast<rapidjson::SizeType>(item.first.size()));
			writer.String(item.second.data(), static_cast<rapidjson::SizeType>(item.second.size()));
		}
		writer.EndObject();
	}
	writer.EndArray();
	SetExtraData(strKey, std::string(buffer.GetString(), buffer.GetSize()));
}
bool CRequest::GetExtraData(const std::string& strKey, std::vector<std::uint64_t>& values) const
{
	values.clear();
	std::string strJson = GetExtraData(strKey);
	rapidjson::Document document;
	document.Parse(strJson.data(), strJson.size());
	if (document.HasParseError() || !document.IsArray() || (4096 < document.Size()))
	{
		return false;
	}
	std::vector<std::uint64_t> parsed;
	parsed.reserve(document.Size());
	for (const auto& value : document.GetArray())
	{
		if (!value.IsUint64() || (0 == value.GetUint64()))
		{
			return false;
		}
		parsed.emplace_back(value.GetUint64());
	}
	values = std::move(parsed);
	return true;
}
bool CRequest::GetExtraData(const std::string& strKey, _TyExtraRows& rows) const
{
	rows.clear();
	std::string strJson = GetExtraData(strKey);
	rapidjson::Document document;
	document.Parse(strJson.data(), strJson.size());
	if (document.HasParseError() || !document.IsArray() || (4096 < document.Size()))
	{
		return false;
	}
	_TyExtraRows parsed;
	parsed.reserve(document.Size());
	for (const auto& value : document.GetArray())
	{
		if (!value.IsObject())
		{
			return false;
		}
		std::unordered_map<std::string, std::string> row;
		for (auto mIter = value.MemberBegin(); value.MemberEnd() != mIter; ++mIter)
		{
			if (!mIter->value.IsString() || !row.emplace(std::string(mIter->name.GetString(), mIter->name.GetStringLength()), std::string(mIter->value.GetString(), mIter->value.GetStringLength())).second)
			{
				return false;
			}
		}
		parsed.emplace_back(std::move(row));
	}
	rows = std::move(parsed);
	return true;
}
void CRequest::SetData(const request::StrategyRuntimeList& value)
{
	m_data->mutable_strategy_runtime_list()->CopyFrom(value);
}

void CRequest::SetData(const request::TradeOrderList& value)
{
	m_data->mutable_trade_orders()->CopyFrom(value);
}

void CRequest::SetData(const request::TradeList& value)
{
	m_data->mutable_trades()->CopyFrom(value);
}

void CRequest::SetData(const request::TradePositionList& value)
{
	m_data->mutable_trade_positions()->CopyFrom(value);
}

void CRequest::SetData(const request::TradeCounterReply& value)
{
	m_data->mutable_counter_reply()->CopyFrom(value);
}

void CRequest::SetData(const request::BrokerList& value)
{
	m_data->mutable_broker_list()->CopyFrom(value);
}

void CRequest::SetEventAccount(const request::TradeAccountInfo& value)
{
	m_data->mutable_event_account()->CopyFrom(value);
}
