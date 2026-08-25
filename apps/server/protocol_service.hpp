#ifndef POKER_SERVER_PROTOCOL_SERVICE_HPP
#define POKER_SERVER_PROTOCOL_SERVICE_HPP

#include "poker/application/idempotency_cache.hpp"
#include "poker/application/blocking_executor.hpp"
#include "poker/application/room_manager.hpp"
#include "poker/cluster/lobby_router.hpp"
#include "poker/config/server_config.hpp"
#include "poker/observability/metrics.hpp"
#include "poker/security/auth_service.hpp"
#include "poker/storage/game_store.hpp"
#include "poker.pb.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace poker::server {

struct ConnectionSession {
    mutable std::mutex mutex;
    std::optional<storage::UserId> user_id;
    std::string username;
    std::string raw_token;
    std::int64_t expires_at_unix_ms{0};
    std::optional<std::uint64_t> table_id;
    bool authentication_in_progress{false};
};

class ProtocolService {
public:
    using Envelope = protocol::v1::Envelope;
    using Send = std::function<void(Envelope)>;
    using Push = std::function<void(storage::UserId, Envelope)>;
    using Schedule = std::function<void(std::chrono::milliseconds, std::function<void()>)>;

    ProtocolService(const config::ServerConfig& config,
                    security::AuthService& auth,
                    security::CryptoProvider& crypto,
                    cluster::NodeRegistry& registry,
                    cluster::LobbyRouter& router,
                    application::RoomManager& rooms,
                    application::BlockingExecutor& storage_executor,
                    storage::GameStore& game_store,
                    observability::MetricsRegistry& metrics,
                    Push push,
                    Schedule schedule);

    void handle(const Envelope& request,
                const std::shared_ptr<ConnectionSession>& session,
                Send send);
    void onDisconnect(const std::shared_ptr<ConnectionSession>& session);

private:
    class TableOperation {
    public:
        TableOperation(ProtocolService* owner, std::uint64_t table_id) noexcept;
        ~TableOperation();
        TableOperation(const TableOperation&) = delete;
        TableOperation& operator=(const TableOperation&) = delete;
        void finish() noexcept;

    private:
        ProtocolService* owner_;
        std::uint64_t table_id_;
    };

    struct Authorized {
        storage::UserId user_id{0};
        bool newly_authenticated{false};
    };

    std::optional<Authorized> authorize(const Envelope& request,
                                        const std::shared_ptr<ConnectionSession>& session,
                                        const Send& send);
    bool beginRequest(storage::UserId user_id,
                      const Envelope& request,
                      const Send& send);
    std::shared_ptr<TableOperation> beginTableOperation(std::uint64_t table_id);
    void finishTableOperation(std::uint64_t table_id) noexcept;
    void reply(storage::UserId user_id,
               const Envelope& request,
               Envelope response,
               const Send& send);
    void fail(const Envelope& request,
              protocol::v1::ErrorCode code,
              std::string message,
              const Send& send,
              std::optional<storage::UserId> user_id = std::nullopt);
    void acknowledge(storage::UserId user_id,
                     const Envelope& request,
                     std::string message,
                     const Send& send);
    Envelope snapshotEnvelope(const Envelope& request,
                              const domain::TableSnapshot& snapshot) const;
    Envelope pushSnapshotEnvelope(std::uint64_t table_id,
                                  const domain::TableSnapshot& snapshot) const;
    void broadcastSnapshot(std::uint64_t table_id,
                           const domain::TableSnapshot& source_snapshot);
    void abortTableAndNotify(std::uint64_t table_id,
                             const domain::TableSnapshot& audit,
                             std::string reason,
                             bool attempt_refund);
    void retryRefund(std::uint64_t table_id,
                     std::uint64_t hand_id,
                     std::uint64_t server_sequence,
                     std::vector<storage::UserId> users,
                     std::size_t attempt);
    void pushTableEvent(std::uint64_t table_id,
                        std::uint64_t hand_id,
                        std::uint64_t server_sequence,
                        const std::vector<storage::UserId>& users,
                        std::string event_type,
                        std::string message);
    void replyWithCurrentSnapshot(std::uint64_t table_id,
                                  storage::UserId user_id,
                                  const Envelope& request,
                                  const Send& send,
                                  bool arm_timeout);
    void scheduleTimeout(std::uint64_t table_id,
                         const domain::TableSnapshot& snapshot);
    static std::int64_t nowUnixMs();
    static protocol::v1::ErrorCode map(domain::TableError error);
    static protocol::v1::ErrorCode map(storage::StorageError error);
    static domain::ActionType map(protocol::v1::ActionType action);

    config::ServerConfig config_;
    security::AuthService& auth_;
    security::CryptoProvider& crypto_;
    cluster::NodeRegistry& registry_;
    cluster::LobbyRouter& router_;
    application::RoomManager& rooms_;
    application::BlockingExecutor& storage_executor_;
    storage::GameStore& game_store_;
    observability::MetricsRegistry& metrics_;
    Push push_;
    Schedule schedule_;
    application::IdempotencyCache idempotency_;
    std::mutex table_operations_mutex_;
    std::unordered_map<std::uint64_t, std::uint64_t> latest_timeout_sequences_;
};

}  // namespace poker::server

#endif
