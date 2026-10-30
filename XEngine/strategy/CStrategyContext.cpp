#include "CStrategyContext.h"

#include "CStrategyService.h"
#include "CTradeService.h"

#include <iostream>

CStrategyContext::CStrategyContext(_TyStrategyId strategyId, const _TyAccount& accountIndex, CStrategyService* pStrategyService, CTradeService* pSnapshotProvider) : m_strategyId(strategyId), m_pStrategyService(pStrategyService), m_accountIndex(accountIndex), m_pSnapshotProvider(pSnapshotProvider)
{
}

COrderSubmitResult CStrategyContext::SubmitOrder(const COrderIntent& intent)
{
	if (!m_bEnabled.load() || (nullptr == m_pStrategyService))
	{
		return { false, false, 0, 0, "Strategy context is disabled" };
	}
	COrderIntent normalizedIntent = intent;
	if (0 == normalizedIntent.m_strategyId)
	{
		normalizedIntent.m_strategyId = m_strategyId;
	}
	if (normalizedIntent.m_strategyId != m_strategyId)
	{
		return { false, false, 0, 0, "Order intent belongs to another strategy" };
	}
	return m_pStrategyService->SubmitOrder(m_strategyId, normalizedIntent);
}

bool CStrategyContext::CancelOrder(_TyOrderId orderId)
{
	return m_bEnabled.load() && (nullptr != m_pStrategyService) && m_pStrategyService->CancelOrder(m_strategyId, orderId);
}

CPositionSnapshot CStrategyContext::GetPosition(const CSecurity& security) const
{
	if (nullptr == m_pSnapshotProvider)
	{
		CPositionSnapshot snapshot;
		snapshot.m_security = security;
		return snapshot;
	}
	return m_pSnapshotProvider->GetPosition(m_accountIndex, security);
}

CAccountSnapshot CStrategyContext::GetAccount() const
{
	if (nullptr == m_pSnapshotProvider)
	{
		return {};
	}
	return m_pSnapshotProvider->GetAccount(m_accountIndex);
}

void CStrategyContext::WriteLog(LogLevel level, const std::string& strMessage)
{
	std::ostream& stream = LogLevel::Error == level ? std::cerr : std::cout;
	stream << "[strategy:" << m_strategyId << "] " << strMessage << '\n';
}

bool CStrategyContext::IsMarketAvailable() const
{
	return (nullptr != m_pStrategyService) && m_pStrategyService->IsMarketAvailable();
}

void CStrategyContext::Disable()
{
	m_bEnabled.store(false);
}
