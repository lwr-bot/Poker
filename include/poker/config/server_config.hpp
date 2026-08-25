#ifndef POKER_CONFIG_SERVER_CONFIG_HPP
#define POKER_CONFIG_SERVER_CONFIG_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace poker::config {

enum class ServerRole : std::uint8_t {
    standalone = 0,
    lobby,
    game,
};

struct MySqlConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{3306};
    std::string user{"poker"};
    std::string password;
    std::string database{"poker"};
    std::size_t pool_size{8};
    std::uint32_t connect_timeout_seconds{5};
};

struct RedisConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{6379};
    std::string password;
    int database{0};
    std::uint32_t connect_timeout_ms{2'000};
};

struct ServerConfig {
    ServerRole role{ServerRole::standalone};
    std::string node_id{"game-1"};
    std::string public_endpoint{"127.0.0.1:7000"};
    std::string listen_host{"0.0.0.0"};
    std::uint16_t listen_port{7000};
    std::string metrics_host{"0.0.0.0"};
    std::uint16_t metrics_port{9100};
    std::size_t io_threads{4};
    std::size_t logic_shards{4};
    std::uint32_t action_timeout_ms{20'000};
    std::uint32_t connection_idle_timeout_ms{45'000};
    std::uint32_t node_failure_grace_ms{15'000};
    std::size_t max_frame_bytes{1024 * 1024};
    std::size_t max_pending_send_bytes{4 * 1024 * 1024};
    std::uint32_t requests_per_second{100};
    std::uint32_t request_burst{200};
    std::uint32_t session_ttl_seconds{86'400};
    MySqlConfig mysql;
    RedisConfig redis;
};

struct ConfigResult {
    std::optional<ServerConfig> value;
    std::vector<std::string> errors;

    explicit operator bool() const noexcept { return value.has_value() && errors.empty(); }
};

ConfigResult loadFromEnvironment();
std::string toString(ServerRole role);

}  // namespace poker::config

#endif
