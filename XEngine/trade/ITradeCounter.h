#pragma once
#include "TradeTypes.h"
#include <functional>

namespace trade
{
	class ITradeCounter
	{
		public:
		using ReplyHandler = std::function<void(const CCounterReply&)>;
		using EventHandler = std::function<void(const CEvent&)>;
		virtual ~ITradeCounter() = default;
		// This receipt means queued only. Business acceptance is delivered in the reply/events.
		virtual CResult Request(CRequest request, ReplyHandler handler) = 0;
		virtual void SetEventHandler(EventHandler handler) = 0;
		virtual CAccountData Snapshot(Id nAccountId) const = 0;
		virtual std::vector<Operation> Capabilities() const = 0;
		virtual void Stop() = 0;
	};
	class ICounterStore
	{
		public:
		virtual ~ICounterStore() = default;
		virtual CResult Acquire(CState& state) = 0;
		// Commit persists a sparse change set plus final metadata, fenced by the previous event sequence.
		virtual CResult Commit(const CState& state, Id nExpectedSequence) = 0;
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
