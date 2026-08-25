#ifndef POKER_CLUSTER_LOBBY_ROUTER_HPP
#define POKER_CLUSTER_LOBBY_ROUTER_HPP

#include "poker/cluster/node_registry.hpp"
#include "poker/security/crypto_provider.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace poker::cluster {

struct RouteAssignment {
    NodeInfo node;
    std::string join_ticket;
};

class LobbyRouter {
public:
    LobbyRouter(NodeRegistry& registry,
                security::CryptoProvider& crypto,
                std::uint32_t room_ttl_seconds = 86'400,
                std::uint32_t ticket_ttl_seconds = 30);

    std::optional<RouteAssignment> assignNewTable(TableId table_id,
                                                   storage::UserId creator_user_id);
    std::optional<RouteAssignment> routeExistingTable(TableId table_id,
                                                       storage::UserId user_id,
                                                       std::string_view expected_node_id = {});

private:
    std::optional<RouteAssignment> issue(const NodeInfo& node,
                                         TableId table_id,
                                         storage::UserId user_id);

    NodeRegistry& registry_;
    security::CryptoProvider& crypto_;
    std::uint32_t room_ttl_seconds_;
    std::uint32_t ticket_ttl_seconds_;
};

}  // namespace poker::cluster

#endif
