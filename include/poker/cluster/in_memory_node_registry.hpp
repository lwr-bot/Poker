#ifndef POKER_CLUSTER_IN_MEMORY_NODE_REGISTRY_HPP
#define POKER_CLUSTER_IN_MEMORY_NODE_REGISTRY_HPP

#include "poker/cluster/node_registry.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace poker::cluster {

class InMemoryNodeRegistry final : public NodeRegistry {
public:
    using Clock = std::function<std::int64_t()>;

    explicit InMemoryNodeRegistry(Clock clock);

    bool ping() override;
    bool heartbeat(const NodeInfo& node, std::uint32_t ttl_seconds) override;
    std::vector<NodeInfo> healthyNodes() override;
    std::optional<TableId> allocateTableId() override;
    bool assignRoom(TableId table_id,
                    std::string_view node_id,
                    std::uint32_t ttl_seconds) override;
    std::optional<std::string> roomOwner(TableId table_id) override;
    bool storeJoinTicket(std::string token_hash,
                         JoinTicket ticket,
                         std::uint32_t ttl_seconds) override;
    std::optional<JoinTicket> consumeJoinTicket(std::string_view token_hash) override;

private:
    template <typename T>
    struct Expiring {
        T value;
        std::int64_t expires_at_ms{0};
    };

    bool nodeIsHealthy(std::string_view node_id, std::int64_t now) const;
    static std::int64_t expiry(std::int64_t now, std::uint32_t ttl_seconds);

    Clock clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, Expiring<NodeInfo>> nodes_;
    std::unordered_map<TableId, Expiring<std::string>> rooms_;
    std::unordered_map<std::string, Expiring<JoinTicket>> tickets_;
    TableId next_table_id_{1'000};
};

}  // namespace poker::cluster

#endif
