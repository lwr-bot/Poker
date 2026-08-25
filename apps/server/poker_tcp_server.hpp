#ifndef POKER_SERVER_POKER_TCP_SERVER_HPP
#define POKER_SERVER_POKER_TCP_SERVER_HPP

#include "poker/config/server_config.hpp"
#include "poker/net/length_field_codec.hpp"
#include "poker/net/token_bucket.hpp"
#include "poker/observability/metrics.hpp"
#include "protocol_service.hpp"

#include <muduo/net/EventLoop.h>
#include <muduo/net/TcpServer.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace poker::server {

class PokerTcpServer {
public:
    PokerTcpServer(muduo::net::EventLoop* loop,
                   const muduo::net::InetAddress& address,
                   const config::ServerConfig& config,
                   ProtocolService& service,
                   observability::MetricsRegistry& metrics);

    void start();
    void push(storage::UserId user_id, ProtocolService::Envelope envelope);

private:
    struct State {
        State(std::size_t max_frame_size,
              double request_rate,
              std::size_t request_burst,
              std::uint64_t connection_generation)
            : codec(max_frame_size),
              limiter(request_rate, request_burst),
              session(std::make_shared<ConnectionSession>()),
              generation(connection_generation),
              last_activity_ms(0) {}

        net::LengthFieldCodec codec;
        net::TokenBucket limiter;
        std::shared_ptr<ConnectionSession> session;
        std::uint64_t generation{0};
        std::atomic<std::int64_t> last_activity_ms;
        std::weak_ptr<muduo::net::TcpConnection> connection;
    };

    struct UserBinding {
        std::weak_ptr<muduo::net::TcpConnection> connection;
        std::uint64_t generation{0};
    };

    void onConnection(const muduo::net::TcpConnectionPtr& connection);
    void onMessage(const muduo::net::TcpConnectionPtr& connection,
                   muduo::net::Buffer* buffer,
                   muduo::Timestamp received_at);
    void send(const muduo::net::TcpConnectionPtr& connection,
              ProtocolService::Envelope envelope);
    void sendProtocolError(const muduo::net::TcpConnectionPtr& connection,
                           protocol::v1::ErrorCode code,
                           std::string message);
    void closeIdleConnections();
    std::shared_ptr<State> stateFor(const muduo::net::TcpConnectionPtr& connection);
    void bindAuthenticated(const muduo::net::TcpConnectionPtr& connection,
                           const std::shared_ptr<State>& state);
    bool unbind(const muduo::net::TcpConnectionPtr& connection,
                const std::shared_ptr<State>& state);

    config::ServerConfig config_;
    muduo::net::TcpServer server_;
    ProtocolService& service_;
    observability::MetricsRegistry& metrics_;
    std::mutex states_mutex_;
    std::unordered_map<const muduo::net::TcpConnection*, std::shared_ptr<State>> states_;
    std::mutex users_mutex_;
    std::unordered_map<storage::UserId, UserBinding> users_;
    std::atomic<std::uint64_t> next_connection_generation_{1};
};

}  // namespace poker::server

#endif
