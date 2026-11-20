#include "StrategyAccountView.h"

CStrategyAccountView MakeStrategyAccountView(trade::CAccountData data, const CSecurity& security)
{
	CStrategyAccountView view;
	view.m_identity = data.m_account.m_account;
	CAccountSnapshot& account = view.m_account;
	account.m_version = data.m_version;
	account.m_time = data.m_time;
	account.m_balance = data.m_account.m_balance;
	account.m_frozen = data.m_account.m_frozen;
	account.m_frozen_known = data.m_account.m_frozen_known;
	bool amountsValid = (0 <= account.m_balance) && (0 <= account.m_frozen) && (account.m_frozen <= account.m_balance);
	account.m_available = data.m_account.m_explicit_available ? data.m_account.m_available : (amountsValid ? account.m_balance - account.m_frozen : 0);
	account.m_ready = data.m_ready && IsAccountValid(data.m_account.m_account) && data.m_account.m_connected && data.m_account.m_funds_valid && (0 <= account.m_available) && (data.m_account.m_explicit_available || (amountsValid && account.m_frozen_known));
	CPositionSnapshot& position = view.m_position;
	position.m_security = security;
	position.m_version = data.m_version;
	position.m_time = data.m_time;
	position.m_ready = account.m_ready;
	position.m_frozen_known = true;
	std::string key = security.String();
	bool found = false;
	for (const auto& row : data.m_positions)
	{
		if (key != row.m_security)
		{
			continue;
		}
		if (found || (data.m_account.m_account != row.m_account))
		{
			position.m_ready = false;
			break;
		}
		found = true;
		position.m_total_quantity = row.m_total;
		position.m_available_quantity = row.m_available;
		position.m_frozen_quantity = row.m_frozen;
		position.m_total_cost = row.m_cost;
		position.m_cost_known = row.m_cost_known && (0 < row.m_total) && (0 < row.m_cost);
		position.m_frozen_known = row.m_frozen_known;
		// Compatibility only. New policies use m_total_cost and m_total_quantity.
		position.m_average_cost = position.m_cost_known ? row.m_cost / row.m_total : 0;
		position.m_ready = position.m_ready && (0 <= row.m_total) && (0 <= row.m_available) && (row.m_available <= row.m_total) && (!row.m_frozen_known || ((0 <= row.m_frozen) && (row.m_frozen <= row.m_total - row.m_available)));
	}
	view.m_orders = std::move(data.m_orders);
	view.m_connection_epoch = data.m_connection_epoch;
	view.m_funds_version = data.m_funds_version;
	view.m_positions_version = data.m_positions_version;
	view.m_orders_version = data.m_orders_version;
	return view;
}
