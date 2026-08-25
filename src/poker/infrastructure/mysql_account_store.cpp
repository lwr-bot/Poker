#include "poker/storage/mysql_account_store.hpp"

#include <mysql/mysql.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace poker::storage {
namespace {

class Statement {
public:
    Statement(MYSQL* connection, const char* sql) : statement_(mysql_stmt_init(connection)) {
        if (statement_ == nullptr
            || mysql_stmt_prepare(statement_, sql, static_cast<unsigned long>(std::strlen(sql))) != 0) {
            if (statement_ != nullptr) {
                mysql_stmt_close(statement_);
                statement_ = nullptr;
            }
        }
    }

    ~Statement() {
        if (statement_ != nullptr) {
            mysql_stmt_close(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    MYSQL_STMT* get() const noexcept { return statement_; }
    explicit operator bool() const noexcept { return statement_ != nullptr; }

private:
    MYSQL_STMT* statement_{nullptr};
};

void bindString(MYSQL_BIND& bind, const std::string& value, unsigned long& length) {
    length = static_cast<unsigned long>(value.size());
    bind.buffer_type = MYSQL_TYPE_STRING;
    bind.buffer = const_cast<char*>(value.data());
    bind.buffer_length = length;
    bind.length = &length;
}

void bindUnsigned64(MYSQL_BIND& bind, std::uint64_t& value) {
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.is_unsigned = true;
}

void bindSigned64(MYSQL_BIND& bind, std::int64_t& value) {
    bind.buffer_type = MYSQL_TYPE_LONGLONG;
    bind.buffer = &value;
    bind.is_unsigned = false;
}

StorageError executeSessionWrite(MySqlConnectionPool& pool,
                                 const char* sql,
                                 std::string token_hash) {
    auto lease = pool.acquire();
    if (!lease) {
        return StorageError::unavailable;
    }
    Statement statement(lease.get(), sql);
    if (!statement) {
        return StorageError::unavailable;
    }
    std::array<MYSQL_BIND, 1> binds{};
    unsigned long token_length = 0;
    bindString(binds[0], token_hash, token_length);
    if (mysql_stmt_bind_param(statement.get(), binds.data()) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        return StorageError::unavailable;
    }
    return mysql_stmt_affected_rows(statement.get()) == 1 ? StorageError::ok
                                                           : StorageError::not_found;
}

StorageResult<UserRecord> fetchUser(MYSQL* connection,
                                    const char* sql,
                                    const std::string* username,
                                    UserId* user_id) {
    Statement statement(connection, sql);
    if (!statement) {
        return {StorageError::unavailable, std::nullopt, "could not prepare user query"};
    }

    std::array<MYSQL_BIND, 1> parameters{};
    unsigned long input_length = 0;
    if (username != nullptr) {
        bindString(parameters[0], *username, input_length);
    } else {
        bindUnsigned64(parameters[0], *user_id);
    }
    if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }

    std::uint64_t id = 0;
    std::array<char, 33> name{};
    std::array<char, 256> password_hash{};
    std::int64_t wallet = 0;
    signed char disabled = 0;
    unsigned long name_length = 0;
    unsigned long password_length = 0;
    std::array<MYSQL_BIND, 5> outputs{};
    bindUnsigned64(outputs[0], id);
    outputs[1].buffer_type = MYSQL_TYPE_STRING;
    outputs[1].buffer = name.data();
    outputs[1].buffer_length = static_cast<unsigned long>(name.size());
    outputs[1].length = &name_length;
    outputs[2].buffer_type = MYSQL_TYPE_STRING;
    outputs[2].buffer = password_hash.data();
    outputs[2].buffer_length = static_cast<unsigned long>(password_hash.size());
    outputs[2].length = &password_length;
    bindSigned64(outputs[3], wallet);
    outputs[4].buffer_type = MYSQL_TYPE_TINY;
    outputs[4].buffer = &disabled;

    if (mysql_stmt_bind_result(statement.get(), outputs.data()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    const auto fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
        return {StorageError::not_found, std::nullopt, "user not found"};
    }
    if (fetched == 1 || fetched == MYSQL_DATA_TRUNCATED) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }

    UserRecord user;
    user.id = id;
    user.username.assign(name.data(), name_length);
    user.password_hash.assign(password_hash.data(), password_length);
    user.wallet_balance = wallet;
    user.disabled = disabled != 0;
    return {StorageError::ok, std::move(user), {}};
}

}  // namespace

MySqlAccountStore::MySqlAccountStore(MySqlConnectionPool& pool) : pool_(pool) {}

StorageResult<UserRecord> MySqlAccountStore::createUser(std::string username,
                                                         std::string password_hash,
                                                         std::int64_t initial_chips) {
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql pool is exhausted"};
    }
    auto* connection = lease.get();
    if (mysql_autocommit(connection, false) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_error(connection)};
    }

    Statement insert_user(connection, "INSERT INTO users(username, password_hash) VALUES (?, ?)");
    std::array<MYSQL_BIND, 2> user_binds{};
    unsigned long username_length = 0;
    unsigned long password_length = 0;
    bindString(user_binds[0], username, username_length);
    bindString(user_binds[1], password_hash, password_length);
    if (!insert_user || mysql_stmt_bind_param(insert_user.get(), user_binds.data()) != 0
        || mysql_stmt_execute(insert_user.get()) != 0) {
        const auto error_number = insert_user ? mysql_stmt_errno(insert_user.get()) : 0;
        const std::string error = insert_user ? mysql_stmt_error(insert_user.get()) : "prepare failed";
        mysql_rollback(connection);
        mysql_autocommit(connection, true);
        return {error_number == 1062 ? StorageError::duplicate : StorageError::unavailable,
                std::nullopt,
                error};
    }

    auto user_id = static_cast<std::uint64_t>(mysql_insert_id(connection));
    Statement insert_wallet(connection, "INSERT INTO wallets(user_id, balance) VALUES (?, ?)");
    std::array<MYSQL_BIND, 2> wallet_binds{};
    bindUnsigned64(wallet_binds[0], user_id);
    bindSigned64(wallet_binds[1], initial_chips);
    if (!insert_wallet || mysql_stmt_bind_param(insert_wallet.get(), wallet_binds.data()) != 0
        || mysql_stmt_execute(insert_wallet.get()) != 0) {
        const std::string error = insert_wallet ? mysql_stmt_error(insert_wallet.get()) : "prepare failed";
        mysql_rollback(connection);
        mysql_autocommit(connection, true);
        return {StorageError::unavailable, std::nullopt, error};
    }

    Statement insert_ledger(
        connection,
        "INSERT INTO wallet_ledger(user_id,idempotency_key,delta_amount,balance_after,reason) "
        "VALUES (?,?,?,?,'initial_grant')");
    const auto initial_key = "initial:" + std::to_string(user_id);
    unsigned long initial_key_length = 0;
    std::array<MYSQL_BIND, 4> ledger_binds{};
    bindUnsigned64(ledger_binds[0], user_id);
    bindString(ledger_binds[1], initial_key, initial_key_length);
    bindSigned64(ledger_binds[2], initial_chips);
    bindSigned64(ledger_binds[3], initial_chips);
    if (!insert_ledger || mysql_stmt_bind_param(insert_ledger.get(), ledger_binds.data()) != 0
        || mysql_stmt_execute(insert_ledger.get()) != 0 || mysql_commit(connection) != 0) {
        const std::string error = insert_ledger ? mysql_stmt_error(insert_ledger.get())
                                                : "prepare failed";
        mysql_rollback(connection);
        mysql_autocommit(connection, true);
        return {StorageError::unavailable, std::nullopt, error};
    }
    mysql_autocommit(connection, true);
    return {StorageError::ok,
            UserRecord{user_id, std::move(username), std::move(password_hash), initial_chips, false},
            {}};
}

StorageResult<UserRecord> MySqlAccountStore::findUserByName(std::string_view username) {
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql pool is exhausted"};
    }
    const std::string name(username);
    return fetchUser(lease.get(),
                     "SELECT u.id,u.username,u.password_hash,w.balance,(u.disabled_at IS NOT NULL) "
                     "FROM users u JOIN wallets w ON w.user_id=u.id WHERE u.username=? LIMIT 1",
                     &name,
                     nullptr);
}

StorageResult<UserRecord> MySqlAccountStore::findUserById(UserId user_id) {
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql pool is exhausted"};
    }
    return fetchUser(lease.get(),
                     "SELECT u.id,u.username,u.password_hash,w.balance,(u.disabled_at IS NOT NULL) "
                     "FROM users u JOIN wallets w ON w.user_id=u.id WHERE u.id=? LIMIT 1",
                     nullptr,
                     &user_id);
}

StorageError MySqlAccountStore::storeSession(SessionRecord session) {
    auto lease = pool_.acquire();
    if (!lease) {
        return StorageError::unavailable;
    }
    Statement statement(lease.get(),
                        "INSERT INTO sessions(user_id,token_hash,last_client_sequence,expires_at) "
                        "VALUES (?,UNHEX(?),?,FROM_UNIXTIME(? / 1000.0))");
    if (!statement) {
        return StorageError::unavailable;
    }
    std::array<MYSQL_BIND, 4> binds{};
    auto user_id = session.user_id;
    auto sequence = session.last_client_sequence;
    auto expires = session.expires_at_unix_ms;
    unsigned long token_length = 0;
    bindUnsigned64(binds[0], user_id);
    bindString(binds[1], session.token_hash, token_length);
    bindUnsigned64(binds[2], sequence);
    bindSigned64(binds[3], expires);
    if (mysql_stmt_bind_param(statement.get(), binds.data()) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        return mysql_stmt_errno(statement.get()) == 1062 ? StorageError::conflict
                                                          : StorageError::unavailable;
    }
    return StorageError::ok;
}

StorageResult<SessionRecord> MySqlAccountStore::findSession(std::string_view token_hash,
                                                             std::int64_t now_unix_ms) {
    auto lease = pool_.acquire();
    if (!lease) {
        return {StorageError::unavailable, std::nullopt, "mysql pool is exhausted"};
    }
    Statement statement(lease.get(),
                        "SELECT user_id,CAST(UNIX_TIMESTAMP(expires_at)*1000 AS UNSIGNED),"
                        "last_client_sequence FROM sessions WHERE token_hash=UNHEX(?) "
                        "AND revoked_at IS NULL AND expires_at>FROM_UNIXTIME(? / 1000.0) LIMIT 1");
    if (!statement) {
        return {StorageError::unavailable, std::nullopt, "could not prepare session query"};
    }
    const std::string token(token_hash);
    unsigned long token_length = 0;
    std::array<MYSQL_BIND, 2> parameters{};
    bindString(parameters[0], token, token_length);
    bindSigned64(parameters[1], now_unix_ms);
    if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0
        || mysql_stmt_execute(statement.get()) != 0
        || mysql_stmt_store_result(statement.get()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }

    std::uint64_t user_id = 0;
    std::uint64_t expires = 0;
    std::uint64_t sequence = 0;
    std::array<MYSQL_BIND, 3> outputs{};
    bindUnsigned64(outputs[0], user_id);
    bindUnsigned64(outputs[1], expires);
    bindUnsigned64(outputs[2], sequence);
    if (mysql_stmt_bind_result(statement.get(), outputs.data()) != 0) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    const auto fetched = mysql_stmt_fetch(statement.get());
    if (fetched == MYSQL_NO_DATA) {
        return {StorageError::not_found, std::nullopt, "session not found"};
    }
    if (fetched == 1) {
        return {StorageError::unavailable, std::nullopt, mysql_stmt_error(statement.get())};
    }
    return {StorageError::ok,
            SessionRecord{user_id, token, static_cast<std::int64_t>(expires), sequence, false},
            {}};
}

StorageError MySqlAccountStore::rotateSession(std::string_view current_token_hash,
                                               SessionRecord replacement,
                                               std::int64_t now_unix_ms) {
    if (replacement.user_id == 0 || replacement.token_hash.empty()
        || replacement.expires_at_unix_ms <= now_unix_ms) {
        return StorageError::invalid_data;
    }
    auto lease = pool_.acquire();
    if (!lease) {
        return StorageError::unavailable;
    }
    Statement statement(
        lease.get(),
        "UPDATE sessions SET token_hash=UNHEX(?),last_client_sequence=?,"
        "expires_at=FROM_UNIXTIME(? / 1000.0) "
        "WHERE token_hash=UNHEX(?) AND user_id=? AND revoked_at IS NULL "
        "AND expires_at>FROM_UNIXTIME(? / 1000.0)");
    if (!statement) {
        return StorageError::unavailable;
    }
    const std::string current(current_token_hash);
    auto sequence = replacement.last_client_sequence;
    auto expires = replacement.expires_at_unix_ms;
    auto user_id = replacement.user_id;
    std::array<MYSQL_BIND, 6> binds{};
    unsigned long replacement_length = 0;
    unsigned long current_length = 0;
    bindString(binds[0], replacement.token_hash, replacement_length);
    bindUnsigned64(binds[1], sequence);
    bindSigned64(binds[2], expires);
    bindString(binds[3], current, current_length);
    bindUnsigned64(binds[4], user_id);
    bindSigned64(binds[5], now_unix_ms);
    if (mysql_stmt_bind_param(statement.get(), binds.data()) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        return mysql_stmt_errno(statement.get()) == 1062 ? StorageError::conflict
                                                          : StorageError::unavailable;
    }
    return mysql_stmt_affected_rows(statement.get()) == 1 ? StorageError::ok
                                                            : StorageError::not_found;
}

StorageError MySqlAccountStore::revokeSession(std::string_view token_hash) {
    return executeSessionWrite(pool_,
                               "UPDATE sessions SET revoked_at=CURRENT_TIMESTAMP(6) "
                               "WHERE token_hash=UNHEX(?) AND revoked_at IS NULL",
                               std::string(token_hash));
}

StorageError MySqlAccountStore::updateSessionSequence(std::string_view token_hash,
                                                       std::uint64_t expected,
                                                       std::uint64_t replacement) {
    auto lease = pool_.acquire();
    if (!lease || replacement != expected + 1) {
        return lease ? StorageError::conflict : StorageError::unavailable;
    }
    Statement statement(lease.get(),
                        "UPDATE sessions SET last_client_sequence=? WHERE token_hash=UNHEX(?) "
                        "AND last_client_sequence=? AND revoked_at IS NULL");
    if (!statement) {
        return StorageError::unavailable;
    }
    const std::string token(token_hash);
    unsigned long token_length = 0;
    std::array<MYSQL_BIND, 3> binds{};
    bindUnsigned64(binds[0], replacement);
    bindString(binds[1], token, token_length);
    bindUnsigned64(binds[2], expected);
    if (mysql_stmt_bind_param(statement.get(), binds.data()) != 0
        || mysql_stmt_execute(statement.get()) != 0) {
        return StorageError::unavailable;
    }
    return mysql_stmt_affected_rows(statement.get()) == 1 ? StorageError::ok
                                                           : StorageError::conflict;
}

}  // namespace poker::storage
