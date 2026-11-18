CREATE TABLE IF NOT EXISTS table_user
(
	user_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	user VARCHAR(64) NOT NULL,
	password_hash BINARY(32) NOT NULL,
	password_salt BINARY(16) NOT NULL,
	status TINYINT UNSIGNED NOT NULL DEFAULT 1,
	created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	PRIMARY KEY (user_id),
	UNIQUE KEY uk_table_user_user (user)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS table_user_watchlist
(
	user_id BIGINT UNSIGNED NOT NULL,
	security VARCHAR(32) NOT NULL,
	created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
	PRIMARY KEY (user_id, security),
	CONSTRAINT fk_table_user_watchlist_user FOREIGN KEY (user_id) REFERENCES table_user(user_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS table_account
(
	user_id BIGINT UNSIGNED NOT NULL,
	broker_id BIGINT UNSIGNED NOT NULL,
	account VARCHAR(64) NOT NULL,
	site VARCHAR(64) NOT NULL,
	broker_name VARCHAR(128) NOT NULL DEFAULT '',
	account_name VARCHAR(128) NOT NULL DEFAULT '',
	password VARCHAR(128) NOT NULL,
	created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	PRIMARY KEY (user_id, broker_id, account),
	KEY idx_table_account_identity (broker_id, account),
	CONSTRAINT fk_table_account_user FOREIGN KEY (user_id) REFERENCES table_user(user_id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS table_strategy
(
	strategy_id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
	config_revision BIGINT UNSIGNED NOT NULL DEFAULT 1,
	account_identity VARCHAR(1024) NOT NULL DEFAULT '',
	strategy_type VARCHAR(64) NOT NULL,
	strategy_name VARCHAR(128) NOT NULL,
	parameters JSON NOT NULL,
	subscriptions JSON NOT NULL,
	event_queue_limit INT UNSIGNED NOT NULL DEFAULT 4096,
	auto_start TINYINT UNSIGNED NOT NULL DEFAULT 0,
	enabled TINYINT UNSIGNED NOT NULL DEFAULT 0,
	created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
	updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
	PRIMARY KEY (strategy_id),
	UNIQUE KEY uk_table_strategy_name (strategy_name),
	KEY idx_table_strategy_enabled (enabled)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

-- Durable strategy execution state. One engine owns this database's execution session.
CREATE TABLE IF NOT EXISTS table_strategy_execution
(
 client_id BIGINT NOT NULL AUTO_INCREMENT,
 account VARBINARY(1024) NOT NULL,
 strategy_id BIGINT NOT NULL,
 signal_id BIGINT NOT NULL,
 security VARCHAR(16) NOT NULL,
 exchange_id INT NOT NULL,
 side INT NOT NULL,
 price BIGINT NOT NULL,
 price_scale INT NOT NULL,
 quantity BIGINT NOT NULL,
 effect INT NOT NULL,
 state INT NOT NULL,
 reserved_money BIGINT NOT NULL,
 order_id BIGINT NOT NULL,
 evidence BIGINT NOT NULL,
 covered TINYINT NOT NULL,
 message TEXT NOT NULL,
 filled BIGINT NOT NULL DEFAULT 0,
 broker_status INT NOT NULL DEFAULT 7,
 PRIMARY KEY (client_id),
 UNIQUE KEY uk_strategy_signal (account,strategy_id,signal_id),
 KEY idx_strategy_pending (state)
) ENGINE=InnoDB AUTO_INCREMENT=1000000000000000 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS table_strategy_protection
(
 account VARBINARY(1024) NOT NULL,
 security VARBINARY(32) NOT NULL,
 owner_id BIGINT NOT NULL,
 reason TEXT NOT NULL,
 PRIMARY KEY (account,security,owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS table_strategy_signal_sequence
(
 id BIGINT NOT NULL AUTO_INCREMENT,
 PRIMARY KEY (id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS table_strategy_checkpoint
(
 strategy_id BIGINT NOT NULL,
 data MEDIUMTEXT NOT NULL,
 PRIMARY KEY (strategy_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE IF NOT EXISTS table_strategy_exit_owner
(
 account VARBINARY(1024) NOT NULL,
 security VARBINARY(32) NOT NULL,
 owner_id BIGINT NOT NULL,
 PRIMARY KEY (account,security)
) ENGINE=InnoDB;
