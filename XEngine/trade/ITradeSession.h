#pragma once
#include "TradeTypes.h"
#include <functional>

namespace trade
{
	class ITradeSession
	{
	public:
		using _TyReplyHandler = std::function<void(const CCounterReply&)>;
		using _TyEventHandler = std::function<void(const CCountEvent&)>;
		virtual ~ITradeSession() = default;
		// Success means queued: complete the callback once. Rejection must not invoke it.
		// Events may precede the reply; business acceptance is delivered in replies/events.
		virtual CResult SendRequest(CReqParam request, _TyReplyHandler handler) = 0;
		virtual CAccountData Snapshot() const = 0;
		virtual void SetEventHandler(_TyEventHandler handler) = 0;
		// Resource cleanup only. Confirm business disconnect through SendRequest first.
		virtual void Close() = 0;
	};
	class ICounterStore
	{
	public:
		virtual ~ICounterStore() = default;
		virtual CResult Acquire(CState& state) = 0;
		// Commit persists a sparse change set plus final metadata, fenced by the previous event sequence.
		virtual CResult Commit(const CState& state, std::uint64_t nExpectedSequence) = 0;
		virtual CResult Renew() = 0;
		virtual void Release() = 0;
	};
	class IMarketInput
	{
	public:
		virtual ~IMarketInput() = default;
		virtual void OnDepth(CDepth depth) = 0;
		virtual void OnMarketState(bool bAvailable) = 0;
	};
} // namespace trade
