#include "poker/storage/mysql_game_store.hpp"
#include "poker/storage/game_store_validation.hpp"

#include <mysql/mysql.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace poker::storage {
namespace {

class GameStatement {
public:
    GameStatement(MYSQL* connection, const char* sql) : value_(mysql_stmt_init(connection)) {
        if (value_ == nullptr
            || mysql_stmt_prepare(value_, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
            if (value_ != nullptr) {
                mysql_stmt_close(value_);
                value_ = nullptr;
            }
        }
    }
    ~GameStatement() {
        if (value_ != nullptr) {
            mysql_stmt_close(value_);
        }
    }
    MYSQL_STMT* get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

private:
    MYSQL_STMT* value_{nullptr};
};

void textBind(MYSQL_BIND& bind, const std::string& value, unsigned long& length) {
    length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = length;
    bind.length = &length;
}

void binaryBind(MYSQL_BIND& bind, const std::string& value, unsigned long& length) {
    textBind(bind, value, length);
    bind.buffer_type = MYSQL_TYPE_BLOB;
}

void unsignedBind(MYSQL_BIND& bind, std::uint64_t& value) {
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.is_unsigned = true;
}

void signedBind(MYSQL_BIND& bind, std::int64_t& value) {
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.is_unsigned = false;
}

void tinyBind(MYSQL_BIND& bind, signed char& value) {
    bind.buffer_type = MYSQL_TYPE_TINY;
    bind.buffer = &value;
}

bool execute(MYSQL_STMT* statement, MYSQL_BIND* binds) {
    return mysql_stmt_bind_param(statement, binds) == 0 && mysql_stmt_execute(statement) == 0;
}

std::optional<std::int64_t> querySigned(MYSQL* connection,
                                        const char* sql,
                                        std::uint64_t first,
                                        std::optional<std::uint64_t> second = std::nullopt) {
    GameStatement statement(connection, sql);
    if (!statement) {
        return std::nullopt;
    }
    bool executed = false;
    if (second.has_value()) {
        std::array<MYSQL_BIND, 2> inputs{};
        unsignedBind(inputs[0], first);
        auto second_value = *second;
        unsignedBind(inputs[1], second_value);
        executed = execute(statement.get(), inputs.data());
    } else {
        std::array<MYSQL_BIND, 1> inputs{};
        unsignedBind(inputs[0], first);
        executed = execute(statement.get(), inputs.data());
    }
    if (!executed || mysql_stmt_store_result(statement.get()) != 0) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    std::array<MYSQL_BIND, 1> output{};
    signedBind(output[0], value);
    if (mysql_stmt_bind_result(statement.get(), output.data()) != 0
        || mysql_stmt_fetch(statement.get()) != 0) {
        return std::nullopt;
    }
    return value;
}

struct LedgerReplay {
    std::string reason;
    std::string reference_id;
    std::int64_t delta{0};
    std::int64_t balance_after{0};
};

std::optional<LedgerReplay> queryLedger(MYSQL* connection,
                                        std::uint64_t user_id,
                                        const std::string& idempotency_key) {
    GameStatement statement(
        connection,
        "SELECT reason,COALESCE(reference_id,''),delta_amount,balance_after "
        "FROM wallet_ledger WHERE user_id=? AND idempotency_key=? LIMIT 1");
    if (!statement) {
        return std::nullopt;
    }
    unsigned long key_length = 0;
    std::array<MYSQL_BIND, 2> inputs{};
    unsignedBind(inputs[0], user_id);
    textBind(inputs[1], idempotency_key, key_length);
    if (!execute(statement.get(), inputs.data()) || mysql_stmt_store_result(statement.get()) != 0) {
        return std::nullopt;
    }

    std::array<char, 32> reason{};
    std::array<char, 97> reference{};
    unsigned long reason_length = 0;
    unsigned long reference_length = 0;
    std::int64_t delta = 0;
    std::int64_t balance_after = 0;
    std::array<MYSQL_BIND, 4> outputs{};
    outputs[0].buffer_type = MYSQL_TYPE_STRING;
    outputs[0].buffer = reason.data();
    outputs[0].buffer_length = static_cast<unsigned long>(reason.size());
    outputs[0].length = &reason_length;
    outputs[1].buffer_type = MYSQL_TYPE_STRING;
    outputs[1].buffer = reference.data();
    outputs[1].buffer_length = static_cast<unsigned long>(reference.size());
    outputs[1].length = &reference_length;
    signedBind(outputs[2], delta);
    signedBind(outputs[3], balance_after);
    if (mysql_stmt_bind_result(statement.get(), outputs.data()) != 0) {
        return std::nullopt;
    }
    const auto fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA || fetched == 1 || fetched == MYSQL_DATA_TRUNCATED) {
        return std::nullopt;
    }
    return LedgerReplay{std::string(reason.data(), reason_length),
                        std::string(reference.data(), reference_length),
                        delta,
                        balance_after};
}

StorageResult<BuyInReceipt> replayBuyIn(const std::optional<LedgerReplay>& replay,
                                        std::uint64_t table_id,
                                        std::int64_t expected_amount) {
    if (!replay.has_value()) {
        return {StorageError::not_found, std::nullopt, {}};
    }
    if (replay->reason != "table_buy_in"
        || replay->reference_id != std::to_string(table_id) || replay->delta >= 0
        || -replay->delta != expected_amount) {
        return {StorageError::conflict, std::nullopt, "idempotency key was reused"};
    }
    return {StorageError::ok,
            BuyInReceipt{replay->balance_after, -replay->delta},
            {}};
}

StorageResult<CashOutReceipt> replayCashOut(const std::optional<LedgerReplay>& replay,
                                            std::uint64_t table_id) {
    if (!replay.has_value()) {
        return {StorageError::not_found, std::nullopt, {}};
    }
    if (replay->reason != "table_cash_out"
        || replay->reference_id != std::to_string(table_id) || replay->delta < 0) {
        return {StorageError::conflict, std::nullopt, "idempotency key was reused"};
    }
    return {StorageError::ok,
            CashOutReceipt{replay->balance_after, replay->delta},
            {}};
}

bool begin(MYSQL* connection) {
    return mysql_autocommit(connection, false) == 0;
}

bool commit(MYSQL* connection) {
    const bool committed = mysql_commit(connection) == 0;
    mysql_autocommit(connection, true);
    return committed;
}

void rollback(MYSQL* connection) {
    mysql_rollback(connection);
    mysql_autocommit(connection, true);
}

std::string encodeCards(const std::vector<domain::Card>& cards) {
    std::string result;
    result.reserve(cards.size());
    for (const auto card : cards) {
        result.push_back(static_cast<char>(domain::cardIndex(card)));
    }
    return result;
}

std::string streetName(domain::Street street) {
    switch (street) {
    case domain::Street::preflop: return "preflop";
    case domain::Street::flop: return "flop";
    case domain::Street::turn: return "turn";
    case domain::Street::river: return "river";
    default: return "preflop";
    }
}

std::string actionName(domain::ActionType action) {
    switch (action) {
    case domain::ActionType::fold: return "fold";
    case domain::ActionType::check: return "check";
    case domain::ActionType::call: return "call";
    case domain::ActionType::bet: return "bet";
    case domain::ActionType::raise: return "raise";
    case domain::ActionType::all_in: return "all_in";
    }
    return "fold";
}

bool isExactActionDuplicate(MYSQL* connection, const HandActionRecord& action) {
    GameStatement statement(
        connection,
        "SELECT action_sequence,request_id,user_id,street,action_type,target_amount "
        "FROM hand_actions WHERE hand_id=? AND "
        "(action_sequence=? OR (user_id=? AND request_id=?)) LIMIT 1");
    if (!statement) {
        return false;
    }
    auto hand_id = action.hand_id;
    auto sequence = action.sequence;
    auto user_id = action.user_id;
    auto request_id = action.request_id;
    std::array<MYSQL_BIND, 4> inputs{};
    unsignedBind(inputs[0], hand_id);
    unsignedBind(inputs[1], sequence);
    unsignedBind(inputs[2], user_id);
    unsignedBind(inputs[3], request_id);
    if (!execute(statement.get(), inputs.data())
        || mysql_stmt_store_result(statement.get()) != 0) {
        return false;
    }

    std::uint64_t stored_sequence = 0;
    std::uint64_t stored_request_id = 0;
    std::uint64_t stored_user_id = 0;
    std::int64_t stored_amount = 0;
    std::array<char, 16> stored_street{};
    std::array<char, 16> stored_action{};
    unsigned long street_length = 0;
    unsigned long action_length = 0;
    std::array<MYSQL_BIND, 6> outputs{};
    unsignedBind(outputs[0], stored_sequence);
    unsignedBind(outputs[1], stored_request_id);
    unsignedBind(outputs[2], stored_user_id);
    outputs[3].buffer_type = MYSQL_TYPE_STRING;
    outputs[3].buffer = stored_street.data();
    outputs[3].buffer_length = static_cast<unsigned long>(stored_street.size());
    outputs[3].length = &street_length;
    outputs[4].buffer_type = MYSQL_TYPE_STRING;
    outputs[4].buffer = stored_action.data();
    outputs[4].buffer_length = static_cast<unsigned long>(stored_action.size());
    outputs[4].length = &action_length;
    signedBind(outputs[5], stored_amount);
    if (mysql_stmt_bind_result(statement.get(), outputs.data()) != 0
        || mysql_stmt_fetch(statement.get()) != 0) {
        return false;
    }
    return stored_sequence == action.sequence && stored_request_id == action.request_id
           && stored_user_id == action.user_id && stored_amount == action.target_amount
           && std::string_view(stored_street.data(), street_length) == streetName(action.street)
           && std::string_view(stored_action.data(), action_length) == actionName(action.action);
}

struct TableRow {
    std::uint64_t id{0};
    std::array<char, 65> name{};
    std::array<char, 65> node{};
    unsigned long name_length{0};
    unsigned long node_length{0};
    std::uint64_t max_players{0};
    std::int64_t small_blind{0};
    std::int64_t big_blind{0};
    std::int64_t min_buy_in{0};
    std::int64_t max_buy_in{0};
    std::uint64_t created_by{0};
    std::array<MYSQL_BIND, 9> binds{};

    void bind() {
        unsignedBind(binds[0], id);
        binds[1].buffer_type = MYSQL_TYPE_STRING;
        binds[1].buffer = name.data();
        binds[1].buffer_length = static_cast<unsigned long>(name.size());
        binds[1].length = &name_length;
        binds[2].buffer_type = MYSQL_TYPE_STRING;
        binds[2].buffer = node.data();
        binds[2].buffer_length = static_cast<unsigned long>(node.size());
        binds[2].length = &node_length;
        unsignedBind(binds[3], max_players);
        signedBind(binds[4], small_blind);
        signedBind(binds[5], big_blind);
        signedBind(binds[6], min_buy_in);
        signedBind(binds[7], max_buy_in);
        unsignedBind(binds[8], created_by);
    }

    TableRecord record() const {
        TableRecord result;
        result.table_id = id;
        result.name.assign(name.data(), name_length);
        result.node_id.assign(node.data(), node_length);
        result.config.min_players = 2;
        result.config.max_players = static_cast<std::size_t>(max_players);
        result.config.small_blind = small_blind;
        result.config.big_blind = big_blind;
        result.config.min_buy_in = min_buy_in;
        result.config.max_buy_in = max_buy_in;
        result.created_by = created_by;
        return result;
    }
};

}  // namespace

MySqlGameStore::MySqlGameStore(MySqlConnectionPool& pool) : pool_(pool) {}

StorageResult<TableRecord> MySqlGameStore::findTable(std::uint64_t table_id) {
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    GameStatement statement(
        lease.get(),
        "SELECT id,name,game_node_id,max_players,small_blind,big_blind,min_buy_in,max_buy_in,"
        "created_by FROM poker_tables WHERE id=? AND status IN ('waiting','playing') LIMIT 1");
    if (!statement) {
        return {StorageError::unavailable, std::nullopt, "table query could not be prepared"};
    }
    std::array<MYSQL_BIND, 1> input{};
    unsignedBind(input[0], table_id);
    if (!execute(statement.get(), input.data()) || mysql_stmt_store_result(statement.get()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    TableRow row;
    row.bind();
    if (mysql_stmt_bind_result(statement.get(), row.binds.data()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    const auto fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
        return {StorageError::not_found, std::nullopt, "table not found"};
    }
    if (fetched == 1 || fetched == MYSQL_DATA_TRUNCATED) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    return {StorageError::ok, row.record(), {}};
}

StorageResult<std::vector<TableRecord>> MySqlGameStore::listOpenTables(std::size_t limit) {
    if (limit == 0) {
        return {StorageError::invalid_data, std::nullopt, "limit must be positive"};
    }
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    GameStatement statement(
        lease.get(),
        "SELECT id,name,game_node_id,max_players,small_blind,big_blind,min_buy_in,max_buy_in,"
        "created_by FROM poker_tables WHERE status IN ('waiting','playing') ORDER BY id LIMIT ?");
    if (!statement) {
        return {StorageError::unavailable, std::nullopt, "table list could not be prepared"};
    }
    auto bounded_limit = static_cast<std::uint64_t>(std::min<std::size_t>(limit, 10'000));
    std::array<MYSQL_BIND, 1> input{};
    unsignedBind(input[0], bounded_limit);
    if (!execute(statement.get(), input.data()) || mysql_stmt_store_result(statement.get()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    TableRow row;
    row.bind();
    if (mysql_stmt_bind_result(statement.get(), row.binds.data()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    std::vector<TableRecord> result;
    while (true) {
        const auto fetched = mysql_stmt_fetch(statement.get());
        if (fetched == MYSQL_NO_DATA) {
            break;
        }
        if (fetched == 1 || fetched == MYSQL_DATA_TRUNCATED) {
            return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
        }
        result.push_back(row.record());
    }
    return {StorageError::ok, std::move(result), {}};
}

StorageResult<std::size_t> MySqlGameStore::countOpenTables(std::string_view node_id) {
    if (node_id.empty()) {
        return {StorageError::invalid_data, std::nullopt, "node id is required"};
    }
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    GameStatement statement(
        lease.get(),
        "SELECT COUNT(*) FROM poker_tables WHERE game_node_id=? "
        "AND status IN ('waiting','playing')");
    if (!statement) {
        return {StorageError::unavailable, std::nullopt, "table count could not be prepared"};
    }
    const std::string node(node_id);
    unsigned long node_length = 0;
    std::array<MYSQL_BIND, 1> input{};
    textBind(input[0], node, node_length);
    if (!execute(statement.get(), input.data()) || mysql_stmt_store_result(statement.get()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    std::uint64_t count = 0;
    std::array<MYSQL_BIND, 1> output{};
    unsignedBind(output[0], count);
    if (mysql_stmt_bind_result(statement.get(), output.data()) != 0
        || mysql_stmt_fetch(statement.get()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    return {StorageError::ok, static_cast<std::size_t>(count), {}};
}

StorageError MySqlGameStore::createTable(const TableRecord& table) {
    auto lease = pool_.acquire();
    if (!lease) {
        return StorageError::unavailable;
    }
    GameStatement statement(
        lease.get(),
        "INSERT INTO poker_tables(id,name,game_node_id,status,max_players,small_blind,big_blind,"
        "min_buy_in,max_buy_in,created_by) VALUES (?,?,?,'waiting',?,?,?,?,?,?)");
    if (!statement) {
        return StorageError::unavailable;
    }
    auto table_id = table.table_id;
    auto max_players = static_cast<std::uint64_t>(table.config.max_players);
    auto small_blind = table.config.small_blind;
    auto big_blind = table.config.big_blind;
    auto min_buy_in = table.config.min_buy_in;
    auto max_buy_in = table.config.max_buy_in;
    auto creator = table.created_by;
    unsigned long name_length = 0;
    unsigned long node_length = 0;
    std::array<MYSQL_BIND, 9> binds{};
    unsignedBind(binds[0], table_id);
    textBind(binds[1], table.name, name_length);
    textBind(binds[2], table.node_id, node_length);
    unsignedBind(binds[3], max_players);
    signedBind(binds[4], small_blind);
    signedBind(binds[5], big_blind);
    signedBind(binds[6], min_buy_in);
    signedBind(binds[7], max_buy_in);
    unsignedBind(binds[8], creator);
    if (execute(statement.get(), binds.data())) {
        return StorageError::ok;
    }
    return mysql_stmt_errno(statement.get()) == 1062 ? StorageError::duplicate
                                                      : StorageError::unavailable;
}

StorageResult<BuyInReceipt> MySqlGameStore::reserveBuyIn(std::uint64_t table_id,
                                                          UserId user_id,
                                                          std::uint32_t seat,
                                                          std::int64_t amount,
                                                          std::string idempotency_key) {
    if (table_id == 0 || user_id == 0 || amount <= 0 || idempotency_key.empty()
        || idempotency_key.size() > 96) {
        return {StorageError::invalid_data, std::nullopt, "invalid buy-in"};
    }
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    const auto existing = replayBuyIn(queryLedger(lease.get(), user_id, idempotency_key),
                                      table_id,
                                      amount);
    if (existing.error != StorageError::not_found) {
        if (existing.error == StorageError::ok) {
            const auto persisted_seat = querySigned(
                lease.get(),
                "SELECT seat_no FROM table_seats WHERE table_id=? AND user_id=?",
                table_id,
                user_id);
            if (!persisted_seat.has_value()
                || *persisted_seat != static_cast<std::int64_t>(seat)) {
                return {StorageError::conflict, std::nullopt,
                        "idempotent buy-in does not match the active seat"};
            }
        }
        return existing;
    }
    if (!begin(lease.get())) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    auto* connection = lease.get();
    GameStatement table_rules(
        connection,
        "SELECT max_players,min_buy_in,max_buy_in FROM poker_tables "
        "WHERE id=? AND status='waiting' FOR UPDATE");
    std::array<MYSQL_BIND, 1> table_input{};
    auto rules_table_id = table_id;
    unsignedBind(table_input[0], rules_table_id);
    if (!table_rules || !execute(table_rules.get(), table_input.data())
        || mysql_stmt_store_result(table_rules.get()) != 0) {
        rollback(connection);
        return {StorageError::unavailable, std::nullopt, "table rules could not be read"};
    }
    std::uint64_t max_players = 0;
    std::int64_t min_buy_in = 0;
    std::int64_t max_buy_in = 0;
    std::array<MYSQL_BIND, 3> table_output{};
    unsignedBind(table_output[0], max_players);
    signedBind(table_output[1], min_buy_in);
    signedBind(table_output[2], max_buy_in);
    if (mysql_stmt_bind_result(table_rules.get(), table_output.data()) != 0) {
        rollback(connection);
        return {StorageError::unavailable, std::nullopt, "table rules could not be decoded"};
    }
    const auto rules_fetched = mysql_stmt_fetch(table_rules.get());
    if (rules_fetched == MYSQL_NO_DATA) {
        rollback(connection);
        return {StorageError::conflict, std::nullopt, "table is missing or a hand is in progress"};
    }
    if (rules_fetched == 1 || rules_fetched == MYSQL_DATA_TRUNCATED) {
        rollback(connection);
        return {StorageError::unavailable, std::nullopt, "table rules could not be decoded"};
    }
    if (static_cast<std::uint64_t>(seat) >= max_players
        || amount < min_buy_in || amount > max_buy_in) {
        rollback(connection);
        return {StorageError::invalid_data, std::nullopt, "seat or buy-in is outside table rules"};
    }

    const auto balance = querySigned(connection,
                                     "SELECT balance FROM wallets WHERE user_id=? FOR UPDATE",
                                     user_id);
    if (!balance.has_value()) {
        rollback(connection);
        return {StorageError::not_found, std::nullopt, "wallet not found"};
    }
    if (*balance < amount) {
        rollback(connection);
        return {StorageError::insufficient_funds, std::nullopt, "insufficient wallet balance"};
    }

    GameStatement insert_seat(connection,
                              "INSERT INTO table_seats(table_id,user_id,seat_no,stack,hand_start_stack) "
                              "VALUES (?,?,?,?,?)");
    auto seat_value = static_cast<std::uint64_t>(seat);
    std::array<MYSQL_BIND, 5> seat_binds{};
    unsignedBind(seat_binds[0], table_id);
    unsignedBind(seat_binds[1], user_id);
    unsignedBind(seat_binds[2], seat_value);
    signedBind(seat_binds[3], amount);
    signedBind(seat_binds[4], amount);
    if (!insert_seat || !execute(insert_seat.get(), seat_binds.data())) {
        const auto duplicate = insert_seat && mysql_stmt_errno(insert_seat.get()) == 1062;
        rollback(connection);
        const auto replay = replayBuyIn(queryLedger(connection, user_id, idempotency_key),
                                        table_id,
                                        amount);
        if (replay.error != StorageError::not_found) {
            return replay;
        }
        return {duplicate ? StorageError::conflict : StorageError::unavailable,
                std::nullopt,
                "seat could not be reserved"};
    }

    GameStatement update_wallet(connection,
                                "UPDATE wallets SET balance=balance-?,version=version+1 "
                                "WHERE user_id=? AND balance>=?");
    std::array<MYSQL_BIND, 3> wallet_binds{};
    signedBind(wallet_binds[0], amount);
    unsignedBind(wallet_binds[1], user_id);
    signedBind(wallet_binds[2], amount);
    if (!update_wallet || !execute(update_wallet.get(), wallet_binds.data())
        || mysql_stmt_affected_rows(update_wallet.get()) != 1) {
        rollback(connection);
        return {StorageError::conflict, std::nullopt, "wallet changed concurrently"};
    }

    const auto after = *balance - amount;
    const auto reference = std::to_string(table_id);
    GameStatement ledger(connection,
                         "INSERT INTO wallet_ledger(user_id,idempotency_key,delta_amount,balance_after,"
                         "reason,reference_id) VALUES (?,?,?,?,'table_buy_in',?)");
    auto delta = -amount;
    unsigned long key_length = 0;
    unsigned long reference_length = 0;
    std::array<MYSQL_BIND, 5> ledger_binds{};
    unsignedBind(ledger_binds[0], user_id);
    textBind(ledger_binds[1], idempotency_key, key_length);
    signedBind(ledger_binds[2], delta);
    auto after_value = after;
    signedBind(ledger_binds[3], after_value);
    textBind(ledger_binds[4], reference, reference_length);
    if (!ledger || !execute(ledger.get(), ledger_binds.data()) || !commit(connection)) {
        rollback(connection);
        return {StorageError::unavailable, std::nullopt, "buy-in ledger commit failed"};
    }
    return {StorageError::ok, BuyInReceipt{after, amount}, {}};
}

StorageResult<CashOutReceipt> MySqlGameStore::cashOut(std::uint64_t table_id,
                                                      UserId user_id,
                                                      std::string idempotency_key) {
    if (table_id == 0 || user_id == 0 || idempotency_key.empty()
        || idempotency_key.size() > 96) {
        return {StorageError::invalid_data, std::nullopt, "invalid cash-out"};
    }
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    const auto existing = replayCashOut(queryLedger(lease.get(), user_id, idempotency_key), table_id);
    if (existing.error != StorageError::not_found) {
        return existing;
    }
    if (!begin(lease.get())) {
        return {StorageError::unavailable, std::nullopt, "mysql is unavailable"};
    }
    auto* connection = lease.get();
    const auto table_waiting = querySigned(
        connection,
        "SELECT status='waiting' FROM poker_tables WHERE id=? FOR UPDATE",
        table_id);
    if (!table_waiting.has_value()) {
        rollback(connection);
        return {StorageError::not_found, std::nullopt, "table not found"};
    }
    if (*table_waiting == 0) {
        rollback(connection);
        return {StorageError::conflict, std::nullopt, "cash-out is not allowed during a hand"};
    }
    const auto stack = querySigned(connection,
                                   "SELECT stack FROM table_seats WHERE table_id=? AND user_id=? FOR UPDATE",
                                   table_id,
                                   user_id);
    const auto balance = querySigned(connection,
                                     "SELECT balance FROM wallets WHERE user_id=? FOR UPDATE",
                                     user_id);
    if (!stack.has_value() || !balance.has_value()) {
        rollback(connection);
        const auto replay = replayCashOut(queryLedger(connection, user_id, idempotency_key), table_id);
        if (replay.error != StorageError::not_found) {
            return replay;
        }
        return {StorageError::not_found, std::nullopt, "seat or wallet not found"};
    }

    GameStatement update_wallet(connection,
                                "UPDATE wallets SET balance=balance+?,version=version+1 WHERE user_id=?");
    std::array<MYSQL_BIND, 2> wallet_binds{};
    auto stack_value = *stack;
    unsignedBind(wallet_binds[1], user_id);
    signedBind(wallet_binds[0], stack_value);
    GameStatement delete_seat(connection,
                              "DELETE FROM table_seats WHERE table_id=? AND user_id=?");
    std::array<MYSQL_BIND, 2> delete_binds{};
    unsignedBind(delete_binds[0], table_id);
    unsignedBind(delete_binds[1], user_id);
    if (!update_wallet || !delete_seat || !execute(update_wallet.get(), wallet_binds.data())
        || !execute(delete_seat.get(), delete_binds.data())) {
        rollback(connection);
        return {StorageError::unavailable, std::nullopt, "cash-out update failed"};
    }

    const auto after = *balance + *stack;
    const auto reference = std::to_string(table_id);
    GameStatement ledger(connection,
                         "INSERT INTO wallet_ledger(user_id,idempotency_key,delta_amount,balance_after,"
                         "reason,reference_id) VALUES (?,?,?,?,'table_cash_out',?)");
    unsigned long key_length = 0;
    unsigned long reference_length = 0;
    std::array<MYSQL_BIND, 5> ledger_binds{};
    unsignedBind(ledger_binds[0], user_id);
    textBind(ledger_binds[1], idempotency_key, key_length);
    signedBind(ledger_binds[2], stack_value);
    auto after_value = after;
    signedBind(ledger_binds[3], after_value);
    textBind(ledger_binds[4], reference, reference_length);
    if (!ledger || !execute(ledger.get(), ledger_binds.data()) || !commit(connection)) {
        rollback(connection);
        return {StorageError::unavailable, std::nullopt, "cash-out ledger commit failed"};
    }
    return {StorageError::ok, CashOutReceipt{after, *stack}, {}};
}

StorageError MySqlGameStore::beginHand(const HandStartRecord& hand) {
    if (hand.hand_id == 0 || hand.table_id == 0 || hand.hand_number == 0
        || hand.players.size() < 2) {
        return StorageError::invalid_data;
    }
    auto lease = pool_.acquire();
    if (!lease || !begin(lease.get())) {
        return StorageError::unavailable;
    }
    auto* connection = lease.get();
    const auto table_waiting = querySigned(
        connection,
        "SELECT status='waiting' FROM poker_tables WHERE id=? FOR UPDATE",
        hand.table_id);
    if (!table_waiting.has_value()) {
        rollback(connection);
        return StorageError::not_found;
    }
    if (*table_waiting == 0) {
        rollback(connection);
        return StorageError::conflict;
    }
    GameStatement insert_hand(
        connection,
        "INSERT INTO hands(id,table_id,hand_number,status,dealer_seat) "
        "VALUES (?,?,?,'playing',?)");
    auto hand_id = hand.hand_id;
    auto table_id = hand.table_id;
    auto hand_number = hand.hand_number;
    auto dealer_seat = static_cast<std::uint64_t>(hand.dealer_seat);
    std::array<MYSQL_BIND, 4> hand_binds{};
    unsignedBind(hand_binds[0], hand_id);
    unsignedBind(hand_binds[1], table_id);
    unsignedBind(hand_binds[2], hand_number);
    unsignedBind(hand_binds[3], dealer_seat);
    if (!insert_hand || !execute(insert_hand.get(), hand_binds.data())) {
        const auto duplicate = insert_hand && mysql_stmt_errno(insert_hand.get()) == 1062;
        rollback(connection);
        return duplicate ? StorageError::duplicate : StorageError::unavailable;
    }

    for (const auto& player : hand.players) {
        if (player.user_id == 0 || player.hole_cards.size() != 2 || player.start_stack < 0) {
            rollback(connection);
            return StorageError::invalid_data;
        }
        const auto hole = encodeCards(player.hole_cards);
        GameStatement insert_player(
            connection,
            "INSERT INTO hand_players(hand_id,user_id,seat_no,start_stack,hole_cards) "
            "VALUES (?,?,?,?,?)");
        auto player_hand_id = hand.hand_id;
        auto user_id = player.user_id;
        auto seat = static_cast<std::uint64_t>(player.seat);
        auto start_stack = player.start_stack;
        unsigned long hole_length = 0;
        std::array<MYSQL_BIND, 5> player_binds{};
        unsignedBind(player_binds[0], player_hand_id);
        unsignedBind(player_binds[1], user_id);
        unsignedBind(player_binds[2], seat);
        signedBind(player_binds[3], start_stack);
        binaryBind(player_binds[4], hole, hole_length);

        GameStatement update_seat(
            connection,
            "UPDATE table_seats SET hand_start_stack=? WHERE table_id=? AND user_id=? AND stack=?");
        auto expected_stack = player.start_stack;
        auto seat_table_id = hand.table_id;
        std::array<MYSQL_BIND, 4> seat_binds{};
        signedBind(seat_binds[0], start_stack);
        unsignedBind(seat_binds[1], seat_table_id);
        unsignedBind(seat_binds[2], user_id);
        signedBind(seat_binds[3], expected_stack);
        if (!insert_player || !update_seat
            || !execute(insert_player.get(), player_binds.data())
            || !execute(update_seat.get(), seat_binds.data())
            || mysql_stmt_affected_rows(update_seat.get()) != 1) {
            rollback(connection);
            return StorageError::conflict;
        }
    }

    GameStatement update_table(connection,
                               "UPDATE poker_tables SET status='playing' "
                               "WHERE id=? AND status='waiting'");
    auto status_table_id = hand.table_id;
    std::array<MYSQL_BIND, 1> table_binds{};
    unsignedBind(table_binds[0], status_table_id);
    if (!update_table || !execute(update_table.get(), table_binds.data())
        || mysql_stmt_affected_rows(update_table.get()) != 1 || !commit(connection)) {
        rollback(connection);
        return StorageError::conflict;
    }
    return StorageError::ok;
}

StorageError MySqlGameStore::appendAction(const HandActionRecord& action) {
    auto lease = pool_.acquire();
    if (!lease) {
        return StorageError::unavailable;
    }
    GameStatement statement(lease.get(),
                            "INSERT INTO hand_actions(hand_id,action_sequence,request_id,user_id,street,"
                            "action_type,target_amount) SELECT ?,?,?,?,?,?,? FROM hands "
                            "WHERE id=? AND status='playing'");
    if (!statement) {
        return StorageError::unavailable;
    }
    auto hand_id = action.hand_id;
    auto sequence = action.sequence;
    auto request_id = action.request_id;
    auto user_id = action.user_id;
    auto amount = action.target_amount;
    const auto street = streetName(action.street);
    const auto action_type = actionName(action.action);
    unsigned long street_length = 0;
    unsigned long action_length = 0;
    auto status_hand_id = action.hand_id;
    std::array<MYSQL_BIND, 8> binds{};
    unsignedBind(binds[0], hand_id);
    unsignedBind(binds[1], sequence);
    unsignedBind(binds[2], request_id);
    unsignedBind(binds[3], user_id);
    textBind(binds[4], street, street_length);
    textBind(binds[5], action_type, action_length);
    signedBind(binds[6], amount);
    unsignedBind(binds[7], status_hand_id);
    if (execute(statement.get(), binds.data())
        && mysql_stmt_affected_rows(statement.get()) == 1) {
        return StorageError::ok;
    }
    if (mysql_stmt_errno(statement.get()) == 1062) {
        return isExactActionDuplicate(lease.get(), action) ? StorageError::duplicate
                                                            : StorageError::conflict;
    }
    return mysql_stmt_errno(statement.get()) == 0 ? StorageError::conflict
                                                   : StorageError::unavailable;
}

StorageError MySqlGameStore::settleHand(const HandSettlementRecord& settlement) {
    if (!validHandSettlement(settlement)) {
        return StorageError::invalid_data;
    }
    auto lease = pool_.acquire();
    if (!lease || !begin(lease.get())) {
        return StorageError::unavailable;
    }
    auto* connection = lease.get();
    const auto table_playing = querySigned(
        connection,
        "SELECT status='playing' FROM poker_tables WHERE id=? FOR UPDATE",
        settlement.table_id);
    if (!table_playing.has_value()) {
        rollback(connection);
        return StorageError::not_found;
    }
    if (*table_playing == 0) {
        rollback(connection);
        return StorageError::conflict;
    }
    const auto board = encodeCards(settlement.board);
    GameStatement update_hand(connection,
                              "UPDATE hands SET status='settled',board_cards=?,total_pot=?,"
                              "settled_at=CURRENT_TIMESTAMP(6) WHERE id=? AND table_id=? "
                              "AND status='playing'");
    auto hand_id = settlement.hand_id;
    auto table_id = settlement.table_id;
    auto total_pot = settlement.total_pot;
    unsigned long board_length = 0;
    std::array<MYSQL_BIND, 4> hand_binds{};
    binaryBind(hand_binds[0], board, board_length);
    signedBind(hand_binds[1], total_pot);
    unsignedBind(hand_binds[2], hand_id);
    unsignedBind(hand_binds[3], table_id);
    if (!update_hand) {
        rollback(connection);
        return StorageError::unavailable;
    }
    if (!execute(update_hand.get(), hand_binds.data())) {
        rollback(connection);
        return StorageError::unavailable;
    }
    if (mysql_stmt_affected_rows(update_hand.get()) != 1) {
        rollback(connection);
        return StorageError::conflict;
    }

    for (const auto& player : settlement.players) {
        const auto hole = encodeCards(player.hole_cards);
        GameStatement update_player(
            connection,
            "UPDATE hand_players SET committed=?,winnings=?,end_stack=?,hole_cards=?,folded=? "
            "WHERE hand_id=? AND user_id=? AND start_stack=?");
        auto player_hand = settlement.hand_id;
        auto user_id = player.user_id;
        auto committed = player.committed;
        auto winnings = player.winnings;
        auto end_stack = player.end_stack;
        signed char folded = player.folded ? 1 : 0;
        unsigned long hole_length = 0;
        auto expected_start_stack = player.start_stack;
        std::array<MYSQL_BIND, 8> player_binds{};
        signedBind(player_binds[0], committed);
        signedBind(player_binds[1], winnings);
        signedBind(player_binds[2], end_stack);
        binaryBind(player_binds[3], hole, hole_length);
        tinyBind(player_binds[4], folded);
        unsignedBind(player_binds[5], player_hand);
        unsignedBind(player_binds[6], user_id);
        signedBind(player_binds[7], expected_start_stack);
        if (!update_player || !execute(update_player.get(), player_binds.data())
            || mysql_stmt_affected_rows(update_player.get()) != 1) {
            rollback(connection);
            return StorageError::conflict;
        }

        GameStatement update_seat(connection,
                                  "UPDATE table_seats SET stack=?,hand_start_stack=?,version=version+1 "
                                  "WHERE table_id=? AND user_id=? AND hand_start_stack=?");
        std::array<MYSQL_BIND, 5> seat_binds{};
        auto final_stack = player.end_stack;
        signedBind(seat_binds[0], final_stack);
        signedBind(seat_binds[1], final_stack);
        unsignedBind(seat_binds[2], table_id);
        unsignedBind(seat_binds[3], user_id);
        signedBind(seat_binds[4], expected_start_stack);
        if (!update_seat || !execute(update_seat.get(), seat_binds.data())
            || mysql_stmt_affected_rows(update_seat.get()) != 1) {
            rollback(connection);
            return StorageError::conflict;
        }
    }

    GameStatement update_table(connection,
                               "UPDATE poker_tables SET status='waiting' WHERE id=?");
    std::array<MYSQL_BIND, 1> table_binds{};
    unsignedBind(table_binds[0], table_id);
    if (!update_table || !execute(update_table.get(), table_binds.data()) || !commit(connection)) {
        rollback(connection);
        return StorageError::unavailable;
    }
    return StorageError::ok;
}

StorageError MySqlGameStore::abortTableAndRefund(std::uint64_t table_id) {
    auto lease = pool_.acquire();
    if (!lease || !begin(lease.get())) {
        return StorageError::unavailable;
    }
    auto* connection = lease.get();
    const auto already_aborted = querySigned(
        connection,
        "SELECT status='aborted' FROM poker_tables WHERE id=? FOR UPDATE",
        table_id);
    if (!already_aborted.has_value()) {
        rollback(connection);
        return StorageError::not_found;
    }
    if (*already_aborted != 0) {
        rollback(connection);
        return StorageError::ok;
    }
    GameStatement update_wallets(
        connection,
        "UPDATE wallets w JOIN table_seats s ON s.user_id=w.user_id SET "
        "w.balance=w.balance+s.stack,w.version=w.version+1 WHERE s.table_id=?");
    std::array<MYSQL_BIND, 1> update_binds{};
    auto update_table_id = table_id;
    unsignedBind(update_binds[0], update_table_id);

    GameStatement ledger(
        connection,
        "INSERT INTO wallet_ledger(user_id,idempotency_key,delta_amount,balance_after,"
        "reason,reference_id) SELECT s.user_id,CONCAT('crash:',?,':',s.user_id),s.stack,w.balance,"
        "'crash_refund',? FROM table_seats s JOIN wallets w ON w.user_id=s.user_id "
        "WHERE s.table_id=?");
    std::array<MYSQL_BIND, 3> ledger_binds{};
    auto key_table_id = table_id;
    auto reference_table_id = table_id;
    auto where_table_id = table_id;
    unsignedBind(ledger_binds[0], key_table_id);
    unsignedBind(ledger_binds[1], reference_table_id);
    unsignedBind(ledger_binds[2], where_table_id);

    GameStatement delete_seats(connection, "DELETE FROM table_seats WHERE table_id=?");
    std::array<MYSQL_BIND, 1> delete_binds{};
    auto delete_table_id = table_id;
    unsignedBind(delete_binds[0], delete_table_id);

    GameStatement abort_hands(
        connection,
        "UPDATE hands SET status='aborted',settled_at=CURRENT_TIMESTAMP(6) "
        "WHERE table_id=? AND status='playing'");
    std::array<MYSQL_BIND, 1> hand_binds{};
    auto hand_table_id = table_id;
    unsignedBind(hand_binds[0], hand_table_id);

    GameStatement close_table(
        connection,
        "UPDATE poker_tables SET status='aborted',closed_at=CURRENT_TIMESTAMP(6) "
        "WHERE id=? AND status IN ('waiting','playing','closing')");
    std::array<MYSQL_BIND, 1> close_binds{};
    auto close_table_id = table_id;
    unsignedBind(close_binds[0], close_table_id);

    if (!update_wallets || !ledger || !delete_seats || !abort_hands || !close_table
        || !execute(update_wallets.get(), update_binds.data())
        || !execute(ledger.get(), ledger_binds.data())
        || !execute(delete_seats.get(), delete_binds.data())
        || !execute(abort_hands.get(), hand_binds.data())
        || !execute(close_table.get(), close_binds.data())
        || mysql_stmt_affected_rows(close_table.get()) != 1 || !commit(connection)) {
        rollback(connection);
        return StorageError::unavailable;
    }
    return StorageError::ok;
}

}  // namespace poker::storage
