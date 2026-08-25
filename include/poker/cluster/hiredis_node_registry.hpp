#ifndef POKER_CLUSTER_HIREDIS_NODE_REGISTRY_HPP
#define POKER_CLUSTER_HIREDIS_NODE_REGISTRY_HPP

#include "poker/cluster/node_registry.hpp"
#include "poker/config/server_config.hpp"

#include <hiredis/hiredis.h>

#include <memory>
#include <mutex>

namespace poker::cluster {

class HiredisNodeRegistry final : public NodeRegistry {
public:
    explicit HiredisNodeRegistry(config::RedisConfig config);
    ~HiredisNodeRegistry() override;

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
    using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

    bool connectLocked();
    Reply commandLocked(const char* format, ...);
    static std::optional<NodeInfo> parseNode(std::string_view node_id, std::string_view value);

    config::RedisConfig config_;
    std::mutex mutex_;
    redisContext* context_{nullptr};
};

}  // namespace poker::cluster

#endif
