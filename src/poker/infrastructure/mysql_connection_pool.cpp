#include "poker/storage/mysql_connection_pool.hpp"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace poker::storage {
namespace {

void initializeMySqlClient() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (mysql_library_init(0, nullptr, nullptr) != 0) {
            throw std::runtime_error("mysql client library initialization failed");
        }
    });
}

}  // namespace

MySqlLease::MySqlLease(MySqlConnectionPool* pool, MYSQL* connection) noexcept
    : pool_(pool), connection_(connection) {}

MySqlLease::~MySqlLease() {
    release();
}

MySqlLease::MySqlLease(MySqlLease&& other) noexcept
    : pool_(other.pool_), connection_(other.connection_) {
    other.pool_ = nullptr;
    other.connection_ = nullptr;
}

MySqlLease& MySqlLease::operator=(MySqlLease&& other) noexcept {
    if (this != &other) {
        release();
        pool_ = other.pool_;
        connection_ = other.connection_;
        other.pool_ = nullptr;
        other.connection_ = nullptr;
    }
    return *this;
}

MYSQL* MySqlLease::get() const noexcept {
    return connection_;
}

MySqlLease::operator bool() const noexcept {
    return connection_ != nullptr;
}

void MySqlLease::release() noexcept {
    if (pool_ != nullptr && connection_ != nullptr) {
        pool_->giveBack(connection_);
    }
    pool_ = nullptr;
    connection_ = nullptr;
}

MySqlConnectionPool::MySqlConnectionPool(const config::MySqlConfig& config)
    : config_(config) {
    initializeMySqlClient();
    all_.reserve(config.pool_size);
    available_.reserve(config.pool_size);
    try {
        for (std::size_t index = 0; index < config.pool_size; ++index) {
            auto* connection = connectOne(config);
            all_.push_back(connection);
            available_.push_back(connection);
        }
    } catch (...) {
        for (auto* connection : all_) {
            mysql_close(connection);
        }
        throw;
    }
}

MySqlConnectionPool::~MySqlConnectionPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto* connection : all_) {
        mysql_close(connection);
    }
}

MySqlLease MySqlConnectionPool::acquire(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] { return stopping_ || !available_.empty(); })) {
        return {};
    }
    if (stopping_) {
        return {};
    }
    auto* connection = available_.back();
    available_.pop_back();
    lock.unlock();
    if (mysql_ping(connection) != 0) {
        if (auto* replacement = replaceConnection(connection)) {
            return {this, replacement};
        }
        returnAvailable(connection);
        return {};
    }
    return {this, connection};
}

bool MySqlConnectionPool::ping(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] { return stopping_ || !available_.empty(); })) {
        return !stopping_ && !all_.empty();
    }
    if (stopping_) {
        return false;
    }
    auto* connection = available_.back();
    available_.pop_back();
    lock.unlock();
    bool healthy = mysql_ping(connection) == 0;
    if (!healthy) {
        if (auto* replacement = replaceConnection(connection)) {
            connection = replacement;
            healthy = true;
        }
    }
    returnAvailable(connection);
    return healthy;
}

std::size_t MySqlConnectionPool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return all_.size();
}

std::size_t MySqlConnectionPool::available() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return available_.size();
}

void MySqlConnectionPool::giveBack(MYSQL* connection) noexcept {
    if (mysql_rollback(connection) != 0 || mysql_autocommit(connection, true) != 0) {
        if (auto* replacement = replaceConnection(connection)) {
            connection = replacement;
        }
    }
    returnAvailable(connection);
}

void MySqlConnectionPool::returnAvailable(MYSQL* connection) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return;
        }
        available_.push_back(connection);
    }
    ready_.notify_one();
}

MYSQL* MySqlConnectionPool::replaceConnection(MYSQL* failed) noexcept {
    MYSQL* replacement = nullptr;
    try {
        replacement = connectOne(config_);
    } catch (...) {
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            mysql_close(replacement);
            return nullptr;
        }
        const auto found = std::find(all_.begin(), all_.end(), failed);
        if (found == all_.end()) {
            mysql_close(replacement);
            return nullptr;
        }
        *found = replacement;
    }
    mysql_close(failed);
    return replacement;
}

MYSQL* MySqlConnectionPool::connectOne(const config::MySqlConfig& config) {
    auto* connection = mysql_init(nullptr);
    if (connection == nullptr) {
        throw std::runtime_error("mysql_init failed");
    }
    const auto timeout = config.connect_timeout_seconds;
    mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(connection, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(connection,
                           config.host.c_str(),
                           config.user.c_str(),
                           config.password.c_str(),
                           config.database.c_str(),
                           config.port,
                           nullptr,
                           0)
        == nullptr) {
        const std::string error = mysql_error(connection);
        mysql_close(connection);
        throw std::runtime_error("mysql connection failed: " + error);
    }
    return connection;
}

}  // namespace poker::storage
