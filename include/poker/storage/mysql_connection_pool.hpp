#ifndef POKER_STORAGE_MYSQL_CONNECTION_POOL_HPP
#define POKER_STORAGE_MYSQL_CONNECTION_POOL_HPP

#include "poker/config/server_config.hpp"

#include <mysql/mysql.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace poker::storage {

class MySqlConnectionPool;

class MySqlLease {
public:
    MySqlLease() = default;
    MySqlLease(MySqlConnectionPool* pool, MYSQL* connection) noexcept;
    ~MySqlLease();

    MySqlLease(const MySqlLease&) = delete;
    MySqlLease& operator=(const MySqlLease&) = delete;
    MySqlLease(MySqlLease&& other) noexcept;
    MySqlLease& operator=(MySqlLease&& other) noexcept;

    MYSQL* get() const noexcept;
    explicit operator bool() const noexcept;

private:
    void release() noexcept;

    MySqlConnectionPool* pool_{nullptr};
    MYSQL* connection_{nullptr};
};

class MySqlConnectionPool {
public:
    explicit MySqlConnectionPool(const config::MySqlConfig& config);
    ~MySqlConnectionPool();

    MySqlConnectionPool(const MySqlConnectionPool&) = delete;
    MySqlConnectionPool& operator=(const MySqlConnectionPool&) = delete;

    MySqlLease acquire(std::chrono::milliseconds timeout = std::chrono::seconds(2));
    bool ping(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    std::size_t size() const;
    std::size_t available() const;

private:
    friend class MySqlLease;
    void giveBack(MYSQL* connection) noexcept;
    void returnAvailable(MYSQL* connection) noexcept;
    MYSQL* replaceConnection(MYSQL* failed) noexcept;
    MYSQL* connectOne(const config::MySqlConfig& config);

    config::MySqlConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::vector<MYSQL*> all_;
    std::vector<MYSQL*> available_;
    bool stopping_{false};
};

}  // namespace poker::storage

#endif
