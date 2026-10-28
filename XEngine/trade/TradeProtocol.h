#pragma once
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../strategy/AccountInfo.h"
#include "TradeTypes.h"
#include "TradingTypes.h"
namespace trade::protocol
{
	request::TradeAccountRecord AccountPayload(const CTradeAccountInfo& info);
	void WriteOrder(const COrder& row, request::TradeOrderRecord& wire);
	void WriteTrade(const CTrade& row, request::TradeRecord& wire);
	void WritePosition(const CPosition& row, request::TradePositionRecord& wire);
	void WriteFunds(const CAccount& row, bool bReady, request::TradeAccountInfo& wire);
	void WriteReply(const CCounterReply& reply, ::CRequest& response);
	void WriteEvent(const CCountEvent& event, request::TradeEventRecord& wire);
	COrderEvent OrderEvent(const CCountEvent& event);
	CTradeEvent TradeEvent(const CCountEvent& event);
	::CRequest WriteRequest(const CReqParam& request);
	bool ReadReply(const ::CRequest& response, CCounterReply& reply);
	bool ReadPush(const ::CRequest& response, CCountEvent& event);
	::CRequest Push(const CCountEvent& event, const CTradeAccountInfo& account);
} // namespace trade::protocol
