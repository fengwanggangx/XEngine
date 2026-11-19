#include "CStrategyContext.h"

#include "CStrategyService.h"
#include "CTradeService.h"

#include <algorithm>
#include "../common/container.h"
#include <iostream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

CStrategyContext::CStrategyContext(_TyStrategyId strategyId, const _TyAccount& account, CStrategyService* pStrategyService, CTradeService* pSnapshotProvider, const CSecurity& security, bool scoped) : m_id(strategyId), m_p_strategy_service(pStrategyService), m_account(account), m_security(security), m_scoped(scoped), m_p_snapshot_provider(pSnapshotProvider)
{
}

COrderSubmitResult CStrategyContext::SubmitOrder(const COrderIntent& intent)
{
	if (!m_b_enabled.load() || (nullptr == m_p_strategy_service))
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
	return m_p_strategy_service->SubmitOrder(m_id, normalizedIntent);
}

bool CStrategyContext::CancelOrder(_TyOrderId orderId)
{
	return m_b_enabled.load() && (nullptr != m_p_strategy_service) && m_p_strategy_service->CancelOrder(m_id, orderId);
}

CPositionSnapshot CStrategyContext::GetPosition(const CSecurity& security) const
{
	if (nullptr == m_p_snapshot_provider)
	{
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		return snapshot;
	}
	return m_p_snapshot_provider->GetPosition(m_account, security);
}

CAccountSnapshot CStrategyContext::GetAccount() const
{
	if (nullptr == m_p_snapshot_provider)
	{
		return {};
	}
	return m_p_snapshot_provider->GetAccount(m_account);
}

void CStrategyContext::WriteLog(LogLevel level, const std::string& strMessage)
{
	std::ostream& stream = LogLevel::Error == level ? std::cerr : std::cout;
	stream << "[strategy:" << m_id << "] " << strMessage << '\n';
}

bool CStrategyContext::IsMarketAvailable() const
{
	return (nullptr != m_p_strategy_service) && m_p_strategy_service->IsMarketAvailable();
}

void CStrategyContext::Disable()
{
	m_b_enabled.store(false);
}

CStrategyAccountView CStrategyContext::GetTradingSnapshot(const CSecurity& security) const
{
	return nullptr == m_p_snapshot_provider ? CStrategyAccountView{} : m_p_snapshot_provider->GetStrategyAccountView(m_account, security);
}

std::optional<trade::CExecutionRule> CStrategyContext::GetRules(const CSecurity& security)
{
	return nullptr == m_p_snapshot_provider ? std::nullopt : m_p_snapshot_provider->GetExecutionRules(m_account, security);
}

std::string CStrategyContext::LoadCheckpoint() const
{
	return m_p_strategy_service->m_execution.LoadCheckpoint(m_id, m_scoped ? m_security.String() : std::string{});
}

std::uint64_t CStrategyContext::AllocateSignal()
{
	return m_p_strategy_service->m_execution.AllocateSignal();
}

std::optional<CExecutionRecord> CStrategyContext::FindExecution(std::uint64_t id) const
{
	return m_p_strategy_service->m_execution.FindExecution(id);
}

std::optional<CExecutionRecord> CStrategyContext::FindSignal(std::uint64_t id) const
{
	return m_p_strategy_service->m_execution.FindSignal(m_id, id);
}

bool CStrategyContext::SaveCheckpoint(const std::string& data, bool protect)
{
	std::shared_ptr<CStrategyRuntime> runtime = m_p_strategy_service->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return false;
	}
	return m_p_strategy_service->m_execution.SaveCheckpoint(m_id, m_account, m_security, data, protect, m_scoped);
}

bool CStrategyContext::ClaimExit()
{
	std::shared_ptr<CStrategyRuntime> runtime = m_p_strategy_service->FindRuntime(m_id);
	return (nullptr != runtime) && m_p_strategy_service->m_execution.ClaimExit(m_account, m_security, m_id);
}

bool CStrategyContext::ReleaseExit(bool unlock)
{
	std::shared_ptr<CStrategyRuntime> runtime = m_p_strategy_service->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return false;
	}
	const CSecurity& security = m_security;
	return m_p_strategy_service->m_execution.ReleaseExit(m_account, security, m_id) && (!unlock || m_p_strategy_service->m_execution.UnlockBuys(m_account, security, m_id));
}

bool CStrategyContext::CancelConflicts(bool reset)
{
	if (reset)
	{
		m_cancelled.clear();
	}
	std::shared_ptr<CStrategyRuntime> runtime = m_p_strategy_service->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return false;
	}
	bool success = true;
	std::string security = m_security.String();
	for (const auto& order : m_p_snapshot_provider->GetOrders(m_account))
	{
		if ((security == order.m_strSecurity) && trade::IsActive(order.m_status) && !order.m_b_cancel_pending && !container::with(m_cancelled, order.m_order_id))
		{
			if (m_p_strategy_service->CancelOrder(order.m_id, order.m_order_id))
			{
				m_cancelled.emplace(order.m_order_id);
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
	return m_p_strategy_service->RequestHistory(m_id, m_security, begin);
}

std::int64_t CStrategyContext::AvailableFunds(const CStrategyAccountView& view) const
{
	return m_p_strategy_service->m_execution.AvailableFunds(m_account, view);
}

void CStrategyContext::PublishBusiness(const std::string& phase, const std::string& details)
{
	std::shared_ptr<CStrategyRuntime> runtime = m_p_strategy_service->FindRuntime(m_id);
	if (nullptr == runtime)
	{
		return;
	}
	std::lock_guard<std::mutex> lock(runtime->m_mtx_metrics);
	if (!m_scoped)
	{
		runtime->m_business_state = phase;
		runtime->m_details_json = details;
		return;
	}
	runtime->m_security_details.insert_or_assign(m_security.String(), std::pair{ phase, details });
	rapidjson::StringBuffer buffer;
	rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
	writer.StartObject();
	writer.Key("securities");
	writer.StartObject();
	for (const auto& [security, status] : runtime->m_security_details)
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
	runtime->m_business_state = std::to_string(runtime->m_securities.size());
	runtime->m_details_json.assign(buffer.GetString(), buffer.GetSize());
}
