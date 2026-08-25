CREATE DATABASE IF NOT EXISTS poker
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_0900_ai_ci;

USE poker;

CREATE TABLE IF NOT EXISTS schema_migrations (
    version             VARCHAR(64) PRIMARY KEY,
    applied_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS users (
    id                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    username            VARCHAR(32) NOT NULL,
    password_hash       VARCHAR(255) NOT NULL,
    created_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    disabled_at         TIMESTAMP(6) NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_users_username (username)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS wallets (
    user_id             BIGINT UNSIGNED NOT NULL,
    balance             BIGINT NOT NULL DEFAULT 100000,
    version             BIGINT UNSIGNED NOT NULL DEFAULT 0,
    updated_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
                        ON UPDATE CURRENT_TIMESTAMP(6),
    PRIMARY KEY (user_id),
    CONSTRAINT fk_wallet_user FOREIGN KEY (user_id) REFERENCES users(id),
    CONSTRAINT chk_wallet_nonnegative CHECK (balance >= 0)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS sessions (
    id                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_id             BIGINT UNSIGNED NOT NULL,
    token_hash          BINARY(32) NOT NULL,
    last_client_sequence BIGINT UNSIGNED NOT NULL DEFAULT 0,
    expires_at          TIMESTAMP(6) NOT NULL,
    revoked_at          TIMESTAMP(6) NULL,
    created_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (id),
    UNIQUE KEY uq_sessions_token_hash (token_hash),
    KEY idx_sessions_user_expiry (user_id, expires_at),
    CONSTRAINT fk_session_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS poker_tables (
    id                  BIGINT UNSIGNED NOT NULL,
    name                VARCHAR(64) NOT NULL,
    game_node_id        VARCHAR(64) NOT NULL,
    status              ENUM('waiting', 'playing', 'closing', 'closed', 'aborted') NOT NULL,
    max_players         TINYINT UNSIGNED NOT NULL,
    small_blind         BIGINT NOT NULL,
    big_blind           BIGINT NOT NULL,
    min_buy_in          BIGINT NOT NULL,
    max_buy_in          BIGINT NOT NULL,
    created_by          BIGINT UNSIGNED NOT NULL,
    created_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    closed_at           TIMESTAMP(6) NULL,
    PRIMARY KEY (id),
    KEY idx_tables_node_status (game_node_id, status),
    CONSTRAINT fk_table_creator FOREIGN KEY (created_by) REFERENCES users(id),
    CONSTRAINT chk_table_players CHECK (max_players BETWEEN 2 AND 6),
    CONSTRAINT chk_table_blinds CHECK (small_blind > 0 AND big_blind >= small_blind * 2),
    CONSTRAINT chk_table_buyin CHECK (min_buy_in > 0 AND max_buy_in >= min_buy_in)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS table_seats (
    table_id            BIGINT UNSIGNED NOT NULL,
    user_id             BIGINT UNSIGNED NOT NULL,
    seat_no             TINYINT UNSIGNED NOT NULL,
    stack               BIGINT NOT NULL,
    hand_start_stack    BIGINT NOT NULL,
    version             BIGINT UNSIGNED NOT NULL DEFAULT 0,
    joined_at           TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
                        ON UPDATE CURRENT_TIMESTAMP(6),
    PRIMARY KEY (table_id, user_id),
    UNIQUE KEY uq_table_seat (table_id, seat_no),
    CONSTRAINT fk_seat_table FOREIGN KEY (table_id) REFERENCES poker_tables(id),
    CONSTRAINT fk_seat_user FOREIGN KEY (user_id) REFERENCES users(id),
    CONSTRAINT chk_seat_stack CHECK (stack >= 0 AND hand_start_stack >= 0)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS hands (
    id                  BIGINT UNSIGNED NOT NULL,
    table_id            BIGINT UNSIGNED NOT NULL,
    hand_number         BIGINT UNSIGNED NOT NULL,
    status              ENUM('playing', 'settled', 'aborted') NOT NULL,
    dealer_seat         TINYINT UNSIGNED NOT NULL,
    board_cards         VARBINARY(10) NULL,
    total_pot           BIGINT NOT NULL DEFAULT 0,
    started_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    settled_at          TIMESTAMP(6) NULL,
    PRIMARY KEY (id),
    UNIQUE KEY uq_table_hand_number (table_id, hand_number),
    KEY idx_hands_table_started (table_id, started_at),
    CONSTRAINT fk_hand_table FOREIGN KEY (table_id) REFERENCES poker_tables(id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS hand_players (
    hand_id             BIGINT UNSIGNED NOT NULL,
    user_id             BIGINT UNSIGNED NOT NULL,
    seat_no             TINYINT UNSIGNED NOT NULL,
    start_stack         BIGINT NOT NULL,
    committed           BIGINT NOT NULL DEFAULT 0,
    winnings            BIGINT NOT NULL DEFAULT 0,
    end_stack           BIGINT NULL,
    hole_cards          VARBINARY(4) NULL,
    folded              BOOLEAN NOT NULL DEFAULT FALSE,
    PRIMARY KEY (hand_id, user_id),
    CONSTRAINT fk_hand_player_hand FOREIGN KEY (hand_id) REFERENCES hands(id),
    CONSTRAINT fk_hand_player_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS hand_actions (
    hand_id             BIGINT UNSIGNED NOT NULL,
    action_sequence     BIGINT UNSIGNED NOT NULL,
    request_id          BIGINT UNSIGNED NOT NULL,
    user_id             BIGINT UNSIGNED NOT NULL,
    street              ENUM('preflop', 'flop', 'turn', 'river') NOT NULL,
    action_type         ENUM('fold', 'check', 'call', 'bet', 'raise', 'all_in') NOT NULL,
    target_amount       BIGINT NOT NULL DEFAULT 0,
    accepted_at         TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (hand_id, action_sequence),
    UNIQUE KEY uq_hand_request (hand_id, user_id, request_id),
    CONSTRAINT fk_action_hand FOREIGN KEY (hand_id) REFERENCES hands(id),
    CONSTRAINT fk_action_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS wallet_ledger (
    id                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    user_id             BIGINT UNSIGNED NOT NULL,
    idempotency_key     VARCHAR(96) NOT NULL,
    delta_amount        BIGINT NOT NULL,
    balance_after       BIGINT NOT NULL,
    reason              ENUM('initial_grant', 'table_buy_in', 'table_cash_out', 'crash_refund', 'admin') NOT NULL,
    reference_id        VARCHAR(96) NULL,
    created_at          TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (id),
    UNIQUE KEY uq_wallet_idempotency (user_id, idempotency_key),
    KEY idx_wallet_ledger_user_created (user_id, created_at),
    CONSTRAINT fk_ledger_user FOREIGN KEY (user_id) REFERENCES users(id),
    CONSTRAINT chk_ledger_balance CHECK (balance_after >= 0)
) ENGINE=InnoDB;

INSERT IGNORE INTO schema_migrations(version) VALUES ('001_schema');

