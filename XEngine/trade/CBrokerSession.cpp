#include "CBrokerSession.h"
#include "CBrokerClient.h"

CBrokerSession::CBrokerSession(std::shared_ptr<CBrokerClient> client, const _TyAccount& account) : m_client(std::move(client)), m_account(account)
{
}

CBrokerSession::~CBrokerSession()
{
	Close();
}

void CBrokerSession::Close()
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	m_closed = true;
	m_handler = {};
}

void CBrokerSession::SetEventHandler(_TyEventHandler handler)
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	if (!m_closed)
	{
		m_handler = std::move(handler);
	}
}

bool CBrokerSession::IsClosed() const
{
	std::lock_guard<std::mutex> lock(m_mtx_state);
	return m_closed;
}

trade::CAccountData CBrokerSession::Snapshot() const
{
	return IsClosed() ? trade::CAccountData{} : m_client->Snapshot(m_account);
}

trade::CResult CBrokerSession::SendRequest(trade::CReqParam request, _TyReplyHandler handler)
{
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (m_closed || m_account != request.m_accountIndex)
		{
			return { trade::Error::Rejected, "closed session or account mismatch" };
		}
	}
	return m_client->SendRequest(std::move(request), std::move(handler));
}

void CBrokerSession::Publish(const trade::CCountEvent& event)
{
	_TyEventHandler handler;
	{
		std::lock_guard<std::mutex> lock(m_mtx_state);
		if (m_closed)
		{
			return;
		}
		handler = m_handler;
	}
	if (nullptr != handler)
	{
		handler(event);
	}
}
