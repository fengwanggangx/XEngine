#include "CStrategyContext.h"

#include "CStrategyService.h"
#include "CTradeService.h"

#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

CStrategyContext::CStrategyContext(_TyStrategyId strategyId, const _TyAccount& account, CStrategyService* pStrategyService, CTradeService* pSnapshotProvider, const CSecurity& security, bool scoped) : m_id(strategyId), m_pStrategyService(pStrategyService), m_account(account), m_security(security), m_scoped(scoped), m_pSnapshotProvider(pSnapshotProvider)
{
}

COrderSubmitResult CStrategyContext::SubmitOrder(const COrderIntent& intent)
{
	if (!m_bEnabled.load() || (nullptr == m_pStrategyService))
	{
		return { false, false, 0, 0, "Strategy context is disabled" };
	}
	COrderIntent normalizedIntent = intent;
	if (0 == normalizedIntent.m_id)
	{
		normalizedIntent.m_id = m_id;
	}
	if (normalizedIntent.m_id != m_id)
	{
		return { false, false, 0, 0, "Order intent belongs to another strategy" };
	}
	return m_pStrategyService->SubmitOrder(m_id, normalizedIntent);
}

bool CStrategyContext::CancelOrder(_TyOrderId orderId)
{
	return m_bEnabled.load() && (nullptr != m_pStrategyService) && m_pStrategyService->CancelOrder(m_id, orderId);
}

CPositionSnapshot CStrategyContext::GetPosition(const CSecurity& security) const
{
	if (nullptr == m_pSnapshotProvider)
	{
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		return snapshot;
	}
	return m_pSnapshotProvider->GetPosition(m_account, security);
}

CAccountSnapshot CStrategyContext::GetAccount() const
{
	if (nullptr == m_pSnapshotProvider)
	{
		return {};
	}
	return m_pSnapshotProvider->GetAccount(m_account);
}

void CStrategyContext::WriteLog(LogLevel level, const std::string& strMessage)
{
	std::ostream& stream = LogLevel::Error == level ? std::cerr : std::cout;
	stream << "[strategy:" << m_id << "] " << strMessage << '\n';
}

bool CStrategyContext::IsMarketAvailable() const
{
	return (nullptr != m_pStrategyService) && m_pStrategyService->IsMarketAvailable();
}

void CStrategyContext::Disable()
{
	m_bEnabled.store(false);
}

CStrategyAccountView CStrategyContext::GetTradingSnapshot(const CSecurity& security) const
{
	return nullptr == m_pSnapshotProvider ? CStrategyAccountView{} : m_pSnapshotProvider->GetStrategyAccountView(m_account, security);
}

std::optional<trade::CExecutionRule> CStrategyContext::GetRules(const CSecurity& security)
{
	return nullptr == m_pSnapshotProvider ? std::nullopt : m_pSnapshotProvider->GetExecutionRules(m_account, security);
}
std::string CStrategyContext::LoadCheckpoint() const
{
	return m_pStrategyService->m_execution.LoadCheckpoint(m_id, m_scoped ? m_security.String() : std::string{});
}
std::uint64_t CStrategyContext::AllocateSignal()
{
	return m_pStrategyService->m_execution.AllocateSignal();
}
std::optional<CExecutionRecord> CStrategyContext::FindExecution(std::uint64_t id) const
{
	return m_pStrategyService->m_execution.FindExecution(id);
}
std::optional<CExecutionRecord> CStrategyContext::FindSignal(std::uint64_t id) const
{
	return m_pStrategyService->m_execution.FindSignal(m_id, id);
}
bool CStrategyContext::SaveCheckpoint(const std::string& data, bool protect)
{
	std::shared_ptr<CStrategyRuntime> runtime = m_pStrategyService->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return false;
	}
	return m_pStrategyService->m_execution.SaveCheckpoint(m_id, m_account, m_security, data, protect, m_scoped);
}
bool CStrategyContext::ClaimExit()
{
	std::shared_ptr<CStrategyRuntime> runtime = m_pStrategyService->FindRuntime(m_id);
	return (nullptr != runtime) && m_pStrategyService->m_execution.ClaimExit(m_account, m_security, m_id);
}
bool CStrategyContext::ReleaseExit(bool unlock)
{
	std::shared_ptr<CStrategyRuntime> runtime = m_pStrategyService->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return false;
	}
	const CSecurity& security = m_security;
	return m_pStrategyService->m_execution.ReleaseExit(m_account, security, m_id) && (!unlock || m_pStrategyService->m_execution.UnlockBuys(m_account, security, m_id));
}
bool CStrategyContext::CancelConflicts(bool reset)
{
	if (reset)
	{
		m_cancelled.clear();
	}
	std::shared_ptr<CStrategyRuntime> runtime = m_pStrategyService->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return false;
	}
	bool success = true;
	for (const auto& order : m_pSnapshotProvider->GetOrders(m_account))
	{
		if ((order.m_strSecurity == m_security.String()) && trade::IsActive(order.m_status) && !order.m_bCancelPending && !m_cancelled.contains(order.m_orderId))
		{
			if (m_pStrategyService->CancelOrder(order.m_id, order.m_orderId))
			{
				m_cancelled.emplace(order.m_orderId);
			}
			else
			{
				success = false;
			}
		}
	}
	return success;
}
bool CStrategyContext::RequestHistory(std::int64_t begin)
{
	return m_pStrategyService->RequestHistory(m_id, m_security, begin);
}
std::int64_t CStrategyContext::AvailableFunds(const CStrategyAccountView& view) const
{
	return m_pStrategyService->m_execution.AvailableFunds(m_account, view);
}
void CStrategyContext::PublishBusiness(const std::string& phase, const std::string& details)
{
	std::shared_ptr<CStrategyRuntime> runtime = m_pStrategyService->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(runtime->m_mtx_metrics);
	if (!m_scoped)
	{
		runtime->m_businessState = phase;
		runtime->m_detailsJson = details;
		return;
	}
	runtime->m_securityDetails.insert_or_assign(m_security.String(), std::pair{ phase, details });
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("securities");
	writer.StartObject();
	for (const auto& [security, status] : runtime->m_securityDetails)
	{
		writer.Key(security.c_str());
		writer.StartObject();
		writer.Key("phase");
		writer.String(status.first.c_str());
		writer.Key("details");
		rapidjson::Document document;
		document.Parse(status.second.c_str());
		document.Accept(writer);
		writer.EndObject();
	}
	writer.EndObject();
	writer.EndObject();
	runtime->m_businessState = std::to_string(runtime->m_securities.size());
	runtime->m_detailsJson.assign(buffer.GetString(), buffer.GetSize());
}
