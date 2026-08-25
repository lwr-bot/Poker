#ifndef POKER_CLUSTER_NODE_REGISTRY_HPP
#define POKER_CLUSTER_NODE_REGISTRY_HPP

#include "poker/storage/account_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace poker::cluster {

using TableId = std::uint64_t;

struct NodeInfo {
    std::string node_id;
    std::string endpoint;
    std::uint64_t active_tables{0};
    std::uint64_t connections{0};
};

struct JoinTicket {
    storage::UserId user_id{0};
    TableId table_id{0};
};

class NodeRegistry {
public:
    virtual ~NodeRegistry() = default;

    virtual bool ping() = 0;
    virtual bool heartbeat(const NodeInfo& node, std::uint32_t ttl_seconds) = 0;
    virtual std::vector<NodeInfo> healthyNodes() = 0;
    virtual std::optional<TableId> allocateTableId() = 0;
    virtual bool assignRoom(TableId table_id,
                            std::string_view node_id,
                            std::uint32_t ttl_seconds) = 0;
    virtual std::optional<std::string> roomOwner(TableId table_id) = 0;
    virtual bool storeJoinTicket(std::string token_hash,
                                 JoinTicket ticket,
                                 std::uint32_t ttl_seconds) = 0;
    virtual std::optional<JoinTicket> consumeJoinTicket(std::string_view token_hash) = 0;
};

}  // namespace poker::cluster

#endif
