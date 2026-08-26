#include "poker_tcp_server.hpp"

#include <muduo/base/Logging.h>
#include <muduo/net/Buffer.h>

#include <functional>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace poker::server {
namespace {

std::int64_t monotonicMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

PokerTcpServer::PokerTcpServer(muduo::net::EventLoop* loop,
                               const muduo::net::InetAddress& address,
                               const config::ServerConfig& config,
                               ProtocolService& service,
                               observability::MetricsRegistry& metrics)
    : config_(config), server_(loop, address, "PokerServer"), service_(service), metrics_(metrics) {
    server_.setThreadNum(static_cast<int>(config_.io_threads));
    server_.setConnectionCallback(
        [this](const auto& connection) { onConnection(connection); });
    server_.setMessageCallback(
        [this](const auto& connection, auto* buffer, auto timestamp) {
            onMessage(connection, buffer, timestamp);
        });
    loop->runEvery(5.0, [this] { closeIdleConnections(); });
}

void PokerTcpServer::start() {
    server_.start();
}

void PokerTcpServer::push(storage::UserId user_id, ProtocolService::Envelope envelope) {
    muduo::net::TcpConnectionPtr connection;
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        const auto found = users_.find(user_id);
        if (found != users_.end()) {
            connection = found->second.connection.lock();
            if (!connection) {
                users_.erase(found);
            }
        }
    }
    if (connection && connection->connected()) {
        send(connection, std::move(envelope));
    }
}

void PokerTcpServer::onConnection(const muduo::net::TcpConnectionPtr& connection) {
    if (connection->connected()) {
        connection->setTcpNoDelay(true);
        metrics_.connectionOpened();
        connection->setHighWaterMarkCallback(
            [this](const auto& slow_connection, std::size_t buffered_bytes) {
                metrics_.rejectedRequest();
                LOG_WARN << "closing slow consumer name=" << slow_connection->name()
                         << " buffered_bytes=" << buffered_bytes;
                slow_connection->forceClose();
            },
            config_.max_pending_send_bytes);
        auto state = std::make_shared<State>(config_.max_frame_bytes,
                                             config_.requests_per_second,
                                             config_.request_burst,
                                             next_connection_generation_.fetch_add(
                                                 1, std::memory_order_relaxed));
        state->connection = connection;
        state->last_activity_ms.store(monotonicMilliseconds(), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(states_mutex_);
            states_[connection.get()] = std::move(state);
        }
        LOG_INFO << "connection opened name=" << connection->name()
                 << " peer=" << connection->peerAddress().toIpPort();
        return;
    }

    std::shared_ptr<State> state;
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        const auto found = states_.find(connection.get());
        if (found != states_.end()) {
            state = found->second;
            states_.erase(found);
        }
    }
    if (state) {
        if (unbind(connection, state)) {
            service_.onDisconnect(state->session);
        }
    }
    metrics_.connectionClosed();
    LOG_INFO << "connection closed name=" << connection->name();
}

void PokerTcpServer::onMessage(const muduo::net::TcpConnectionPtr& connection,
                               muduo::net::Buffer* buffer,
                               muduo::Timestamp received_at) {
    static_cast<void>(received_at);
    const auto state = stateFor(connection);
    if (!state) {
        connection->shutdown();
        return;
    }
    state->last_activity_ms.store(monotonicMilliseconds(), std::memory_order_relaxed);

    const auto bytes = buffer->retrieveAllAsString();
    const auto decoded = state->codec.feed(bytes);
    if (!decoded) {
        metrics_.invalidFrame();
        sendProtocolError(connection,
                          protocol::v1::INVALID_MESSAGE,
                          "invalid TCP frame: " + net::toString(decoded.error));
        connection->shutdown();
        return;
    }

    for (const auto& frame : decoded.frames) {
        if (!state->limiter.allow()) {
            metrics_.rejectedRequest();
            sendProtocolError(connection,
                              protocol::v1::RATE_LIMITED,
                              "connection request rate exceeded");
            continue;
        }
        metrics_.requestReceived();
        const auto started = std::chrono::steady_clock::now();
        ProtocolService::Envelope request;
        if (!request.ParseFromArray(frame.data(), static_cast<int>(frame.size()))) {
            metrics_.invalidFrame();
            sendProtocolError(connection,
                              protocol::v1::INVALID_MESSAGE,
                              "payload is not a valid protobuf Envelope");
            continue;
        }
        service_.handle(request,
                        state->session,
                        [this, connection, state, started](auto envelope) {
                            metrics_.observeRequestLatency(
                                std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - started));
                            bindAuthenticated(connection, state);
                            send(connection, std::move(envelope));
                        });
        bindAuthenticated(connection, state);
    }
}

void PokerTcpServer::send(const muduo::net::TcpConnectionPtr& connection,
                          ProtocolService::Envelope envelope) {
    if (!connection || !connection->connected()) {
        return;
    }
    std::string payload;
    if (!envelope.SerializeToString(&payload)) {
        LOG_ERROR << "protobuf serialization failed connection=" << connection->name();
        return;
    }
    try {
        const auto frame = net::LengthFieldCodec::encode(payload, config_.max_frame_bytes);
        connection->send(std::string(reinterpret_cast<const char*>(frame.data()), frame.size()));
        metrics_.responseSent();
    } catch (const std::exception& error) {
        LOG_ERROR << "frame encoding failed connection=" << connection->name()
                  << " error=" << error.what();
        connection->shutdown();
    }
}

void PokerTcpServer::sendProtocolError(const muduo::net::TcpConnectionPtr& connection,
                                       protocol::v1::ErrorCode code,
                                       std::string message) {
    ProtocolService::Envelope response;
    response.set_protocol_version(1);
    response.set_message_type(protocol::v1::ERROR_RESPONSE);
    response.mutable_error_response()->set_code(code);
    response.mutable_error_response()->set_message(std::move(message));
    send(connection, std::move(response));
}

void PokerTcpServer::closeIdleConnections() {
    const auto now = monotonicMilliseconds();
    std::vector<muduo::net::TcpConnectionPtr> idle;
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        for (const auto& entry : states_) {
            const auto& state = entry.second;
            if (now - state->last_activity_ms.load(std::memory_order_relaxed)
                < static_cast<std::int64_t>(config_.connection_idle_timeout_ms)) {
                continue;
            }
            if (auto connection = state->connection.lock()) {
                idle.push_back(std::move(connection));
            }
        }
    }
    for (const auto& connection : idle) {
        LOG_WARN << "closing idle connection name=" << connection->name();
        sendProtocolError(connection, protocol::v1::SERVICE_UNAVAILABLE,
                          "connection heartbeat timed out");
        connection->shutdown();
    }
}

std::shared_ptr<PokerTcpServer::State> PokerTcpServer::stateFor(
    const muduo::net::TcpConnectionPtr& connection) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    const auto found = states_.find(connection.get());
    return found == states_.end() ? nullptr : found->second;
}

void PokerTcpServer::bindAuthenticated(const muduo::net::TcpConnectionPtr& connection,
                                       const std::shared_ptr<State>& state) {
    if (!connection || !connection->connected()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(states_mutex_);
        const auto found = states_.find(connection.get());
        if (found == states_.end() || found->second.get() != state.get()) {
            return;
        }
    }
    std::optional<storage::UserId> user_id;
    {
        std::lock_guard<std::mutex> lock(state->session->mutex);
        user_id = state->session->user_id;
    }
    if (!user_id.has_value()) {
        std::lock_guard<std::mutex> lock(users_mutex_);
        for (auto binding = users_.begin(); binding != users_.end();) {
            const auto bound_connection = binding->second.connection.lock();
            if (binding->second.generation == state->generation
                && (!bound_connection || bound_connection.get() == connection.get())) {
                binding = users_.erase(binding);
            } else {
                ++binding;
            }
        }
        return;
    }
    muduo::net::TcpConnectionPtr previous;
    {
        std::lock_guard<std::mutex> lock(users_mutex_);
        for (auto binding = users_.begin(); binding != users_.end();) {
            const auto bound_connection = binding->second.connection.lock();
            if (binding->first != *user_id
                && binding->second.generation == state->generation
                && (!bound_connection || bound_connection.get() == connection.get())) {
                binding = users_.erase(binding);
            } else {
                ++binding;
            }
        }
        const auto found = users_.find(*user_id);
        if (found != users_.end()) {
            if (found->second.generation > state->generation) {
                return;
            }
            previous = found->second.connection.lock();
        }
        users_[*user_id] = UserBinding{connection, state->generation};
    }
    if (previous && previous.get() != connection.get() && previous->connected()) {
        LOG_WARN << "closing superseded connection user=" << *user_id;
        ProtocolService::Envelope replaced;
        replaced.set_protocol_version(1);
        replaced.set_message_type(protocol::v1::TABLE_EVENT);
        replaced.mutable_table_event()->set_event_type("session_replaced");
        replaced.mutable_table_event()->set_event_payload(
            "this session was replaced by a newer connection");
        send(previous, std::move(replaced));
        previous->shutdown();
    }
}

bool PokerTcpServer::unbind(const muduo::net::TcpConnectionPtr& connection,
                            const std::shared_ptr<State>& state) {
    std::optional<storage::UserId> user_id;
    {
        std::lock_guard<std::mutex> lock(state->session->mutex);
        user_id = state->session->user_id;
    }
    if (!user_id.has_value()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(users_mutex_);
    const auto found = users_.find(*user_id);
    if (found == users_.end()) {
        return true;
    }
    const auto current = found->second.connection.lock();
    const bool owns_binding = !current || (current.get() == connection.get()
                                           && found->second.generation == state->generation);
    if (owns_binding) {
        users_.erase(found);
    }
    return owns_binding;
}

}  // namespace poker::server
