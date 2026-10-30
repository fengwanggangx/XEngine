#pragma once
#include "ITradeSession.h"
#include <memory>
#include <mutex>

class CBrokerClient;

class CBrokerSession final : public trade::ITradeSession
{
public:
	CBrokerSession(std::shared_ptr<CBrokerClient> client, const _TyAccount& account);
	~CBrokerSession() override;
	void Close() override;
	bool IsClosed() const;
	trade::CAccountData Snapshot() const override;
	void SetEventHandler(_TyEventHandler handler) override;
	trade::CResult SendRequest(trade::CReqParam request, _TyReplyHandler handler) override;
	void Publish(const trade::CCountEvent& event);

private:
	std::shared_ptr<CBrokerClient> m_client;
	_TyAccount m_account;
	mutable std::mutex m_mtx_state;
	_TyEventHandler m_handler;
	bool m_closed{ false };
};
