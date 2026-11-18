#include "StrategyAccountView.h"

CStrategyAccountView MakeStrategyAccountView(trade::CAccountData data, const CSecurity& security)
{
	CStrategyAccountView view;
	view.m_identity = data.m_account.m_account;
	CAccountSnapshot& account = view.m_account;
	account.m_version = data.m_version;
	account.m_nTime = data.m_nTime;
	account.m_balance = data.m_account.m_nBalance;
	account.m_frozen = data.m_account.m_nFrozen;
	account.m_bFrozenKnown = data.m_account.m_bFrozenKnown;
	bool amountsValid = (0 <= account.m_balance) && (0 <= account.m_frozen) && (account.m_frozen <= account.m_balance);
	account.m_available = data.m_account.m_bExplicitAvailable ? data.m_account.m_nAvailable : (amountsValid ? account.m_balance - account.m_frozen : 0);
	account.m_bReady = data.m_bReady && IsAccountValid(data.m_account.m_account) && data.m_account.m_bConnected && data.m_account.m_bFundsValid && (0 <= account.m_available) && (data.m_account.m_bExplicitAvailable || (amountsValid && account.m_bFrozenKnown));
	CPositionSnapshot& position = view.m_position;
	position.m_security = security;
	position.m_version = data.m_version;
	position.m_nTime = data.m_nTime;
	position.m_bReady = account.m_bReady;
	position.m_bFrozenKnown = true;
	std::string key = security.String();
	bool found = false;
	for (const auto& row : data.m_positions)
	{
		if (key != row.m_strSecurity)
		{
			continue;
		}
		if (found || (data.m_account.m_account != row.m_account))
		{
			position.m_bReady = false;
			break;
		}
		found = true;
		position.m_totalQuantity = row.m_nTotal;
		position.m_availableQuantity = row.m_nAvailable;
		position.m_frozenQuantity = row.m_nFrozen;
		position.m_totalCost = row.m_nCost;
		position.m_bCostKnown = row.m_bCostKnown && (0 < row.m_nTotal) && (0 < row.m_nCost);
		position.m_bFrozenKnown = row.m_bFrozenKnown;
		// Compatibility only. New policies use m_totalCost and m_totalQuantity.
		position.m_averageCost = position.m_bCostKnown ? row.m_nCost / row.m_nTotal : 0;
		position.m_bReady = position.m_bReady && (0 <= row.m_nTotal) && (0 <= row.m_nAvailable) && (row.m_nAvailable <= row.m_nTotal) && (!row.m_bFrozenKnown || ((0 <= row.m_nFrozen) && (row.m_nFrozen <= row.m_nTotal - row.m_nAvailable)));
	}
	view.m_orders = std::move(data.m_orders);
	view.m_fundsVersion = data.m_fundsVersion;
	view.m_positionsVersion = data.m_positionsVersion;
	view.m_ordersVersion = data.m_ordersVersion;
	return view;
}
