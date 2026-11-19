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
	account.m_b_frozen_known = data.m_account.m_b_frozen_known;
	bool amountsValid = (0 <= account.m_balance) && (0 <= account.m_frozen) && (account.m_frozen <= account.m_balance);
	account.m_available = data.m_account.m_b_explicit_available ? data.m_account.m_nAvailable : (amountsValid ? account.m_balance - account.m_frozen : 0);
	account.m_bReady = data.m_bReady && IsAccountValid(data.m_account.m_account) && data.m_account.m_bConnected && data.m_account.m_b_funds_valid && (0 <= account.m_available) && (data.m_account.m_b_explicit_available || (amountsValid && account.m_b_frozen_known));
	CPositionSnapshot& position = view.m_position;
	position.m_security = security;
	position.m_version = data.m_version;
	position.m_nTime = data.m_nTime;
	position.m_bReady = account.m_bReady;
	position.m_b_frozen_known = true;
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
		position.m_total_quantity = row.m_nTotal;
		position.m_available_quantity = row.m_nAvailable;
		position.m_frozen_quantity = row.m_nFrozen;
		position.m_total_cost = row.m_nCost;
		position.m_b_cost_known = row.m_b_cost_known && (0 < row.m_nTotal) && (0 < row.m_nCost);
		position.m_b_frozen_known = row.m_b_frozen_known;
		// Compatibility only. New policies use m_totalCost and m_totalQuantity.
		position.m_average_cost = position.m_b_cost_known ? row.m_nCost / row.m_nTotal : 0;
		position.m_bReady = position.m_bReady && (0 <= row.m_nTotal) && (0 <= row.m_nAvailable) && (row.m_nAvailable <= row.m_nTotal) && (!row.m_b_frozen_known || ((0 <= row.m_nFrozen) && (row.m_nFrozen <= row.m_nTotal - row.m_nAvailable)));
	}
	view.m_orders = std::move(data.m_orders);
	view.m_connection_epoch = data.m_connection_epoch;
	view.m_funds_version = data.m_funds_version;
	view.m_positions_version = data.m_positions_version;
	view.m_orders_version = data.m_orders_version;
	return view;
}
