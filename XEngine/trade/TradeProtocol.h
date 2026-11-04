#pragma once
#include "../request/request.h"
#include "../request/request.pb.h"
#include "../strategy/AccountInfo.h"
#include "TradeTypes.h"
#include "TradingTypes.h"
namespace trade::protocol
{
	request::TradeAccountRecord AccountPayload(const CAccountInfo& info);
	void WriteOrder(const COrder& row, request::TradeOrderRecord& wire);
	void WriteTrade(const CTrade& row, request::TradeRecord& wire);
	void WritePosition(const CPosition& row, request::TradePositionRecord& wire);
	void WriteFunds(const CFunds& row, bool bReady, request::TradeAccountInfo& wire);
	void WriteReply(const CCounterReply& reply, ::CRequest& response);
	void WriteEvent(const CCounterEvent& event, request::TradeEventRecord& wire);
	COrderEvent OrderEvent(const CCounterEvent& event);
	CTradeEvent TradeEvent(const CCounterEvent& event);
	::CRequest WriteRequest(const CReqParam& request);
	bool ReadReply(const ::CRequest& response, CCounterReply& reply);
	bool ReadPush(const ::CRequest& response, CCounterEvent& event);
	::CRequest Push(const CCounterEvent& event, const CAccountInfo& account);
} // namespace trade::protocol
