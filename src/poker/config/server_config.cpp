#include "poker/config/server_config.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace poker::config {
namespace {

std::optional<std::string> environment(const char* name) {
    const auto* value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

std::string text(const char* name, std::string fallback) {
    const auto value = environment(name);
    return value.has_value() ? *value : std::move(fallback);
}

template <typename Integer>
Integer integer(const char* name,
                Integer fallback,
                Integer minimum,
                Integer maximum,
                std::vector<std::string>& errors) {
    const auto value = environment(name);
    if (!value.has_value()) {
        return fallback;
    }
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoll(*value, &consumed, 10);
        if (consumed != value->size() || parsed < static_cast<long long>(minimum)
            || parsed > static_cast<long long>(maximum)) {
            throw std::out_of_range("outside allowed range");
        }
        return static_cast<Integer>(parsed);
    } catch (const std::exception&) {
        errors.push_back(std::string(name) + " must be an integer in ["
                         + std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
        return fallback;
    }
}

ServerRole role(std::vector<std::string>& errors) {
    const auto value = text("POKER_ROLE", "standalone");
    if (value == "standalone") {
        return ServerRole::standalone;
    }
    if (value == "lobby") {
        return ServerRole::lobby;
    }
    if (value == "game") {
        return ServerRole::game;
    }
    errors.push_back("POKER_ROLE must be standalone, lobby, or game");
    return ServerRole::standalone;
}

}  // namespace

ConfigResult loadFromEnvironment() {
    ConfigResult result;
    ServerConfig config;
    config.role = role(result.errors);
    config.node_id = text("POKER_NODE_ID", config.node_id);
    config.public_endpoint = text("POKER_PUBLIC_ENDPOINT", config.public_endpoint);
    config.listen_host = text("POKER_LISTEN_HOST", config.listen_host);
    config.listen_port = integer<std::uint16_t>("POKER_LISTEN_PORT", 7000, 1, 65'535, result.errors);
    config.metrics_host = text("POKER_METRICS_HOST", config.metrics_host);
    config.metrics_port = integer<std::uint16_t>("POKER_METRICS_PORT", 9100, 1, 65'535, result.errors);
    config.io_threads = integer<std::size_t>("POKER_IO_THREADS", 4, 1, 128, result.errors);
    config.logic_shards = integer<std::size_t>("POKER_LOGIC_SHARDS", 4, 1, 128, result.errors);
    config.action_timeout_ms = integer<std::uint32_t>(
        "POKER_ACTION_TIMEOUT_MS", 20'000, 1'000, 300'000, result.errors);
    config.connection_idle_timeout_ms = integer<std::uint32_t>(
        "POKER_CONNECTION_IDLE_TIMEOUT_MS", 45'000, 10'000, 600'000, result.errors);
    config.node_failure_grace_ms = integer<std::uint32_t>(
        "POKER_NODE_FAILURE_GRACE_MS", 15'000, 6'000, 300'000, result.errors);
    config.max_frame_bytes = integer<std::size_t>(
        "POKER_MAX_FRAME_BYTES", 1024 * 1024, 1024, 16 * 1024 * 1024, result.errors);
    config.max_pending_send_bytes = integer<std::size_t>(
        "POKER_MAX_PENDING_SEND_BYTES", 4 * 1024 * 1024,
        64 * 1024, 64 * 1024 * 1024, result.errors);
    config.requests_per_second = integer<std::uint32_t>(
        "POKER_REQUESTS_PER_SECOND", 100, 1, 1'000'000, result.errors);
    config.request_burst = integer<std::uint32_t>(
        "POKER_REQUEST_BURST", 200, 1, 1'000'000, result.errors);
    config.session_ttl_seconds = integer<std::uint32_t>(
        "POKER_SESSION_TTL_SECONDS", 86'400, 300, 30 * 24 * 60 * 60, result.errors);

    config.mysql.host = text("POKER_MYSQL_HOST", config.mysql.host);
    config.mysql.port = integer<std::uint16_t>("POKER_MYSQL_PORT", 3306, 1, 65'535, result.errors);
    config.mysql.user = text("POKER_MYSQL_USER", config.mysql.user);
    config.mysql.password = text("POKER_MYSQL_PASSWORD", {});
    config.mysql.database = text("POKER_MYSQL_DATABASE", config.mysql.database);
    config.mysql.pool_size = integer<std::size_t>("POKER_MYSQL_POOL_SIZE", 8, 1, 128, result.errors);

    config.redis.host = text("POKER_REDIS_HOST", config.redis.host);
    config.redis.port = integer<std::uint16_t>("POKER_REDIS_PORT", 6379, 1, 65'535, result.errors);
    config.redis.password = text("POKER_REDIS_PASSWORD", {});
    config.redis.database = integer<int>("POKER_REDIS_DATABASE", 0, 0, 15, result.errors);

    if (config.node_id.empty()) {
        result.errors.push_back("POKER_NODE_ID must not be empty");
    }
    if (config.public_endpoint.empty()) {
        result.errors.push_back("POKER_PUBLIC_ENDPOINT must not be empty");
    }
    if (config.metrics_host.empty()) {
        result.errors.push_back("POKER_METRICS_HOST must not be empty");
    }
    if (config.mysql.password.empty()) {
        result.errors.push_back("POKER_MYSQL_PASSWORD must be provided; credentials are never compiled in");
    }
    if (result.errors.empty()) {
        result.value = std::move(config);
    }
    return result;
}

std::string toString(ServerRole role) {
    switch (role) {
    case ServerRole::standalone: return "standalone";
    case ServerRole::lobby: return "lobby";
    case ServerRole::game: return "game";
    }
    return "unknown";
}

}  // namespace poker::config
