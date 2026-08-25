#include "poker/cluster/lobby_router.hpp"

#include <algorithm>

namespace poker::cluster {

LobbyRouter::LobbyRouter(NodeRegistry& registry,
                         security::CryptoProvider& crypto,
                         std::uint32_t room_ttl_seconds,
                         std::uint32_t ticket_ttl_seconds)
    : registry_(registry),
      crypto_(crypto),
      room_ttl_seconds_(room_ttl_seconds),
      ticket_ttl_seconds_(ticket_ttl_seconds) {}

std::optional<RouteAssignment> LobbyRouter::assignNewTable(TableId table_id,
                                                            storage::UserId creator_user_id) {
    const auto nodes = registry_.healthyNodes();
    if (table_id == 0 || creator_user_id == 0 || nodes.empty()) {
        return std::nullopt;
    }
    const auto& node = nodes.front();
    if (!registry_.assignRoom(table_id, node.node_id, room_ttl_seconds_)) {
        return std::nullopt;
    }
    return issue(node, table_id, creator_user_id);
}

std::optional<RouteAssignment> LobbyRouter::routeExistingTable(TableId table_id,
                                                                storage::UserId user_id,
                                                                std::string_view expected_node_id) {
    if (table_id == 0 || user_id == 0) {
        return std::nullopt;
    }
    auto owner = registry_.roomOwner(table_id);
    const auto nodes = registry_.healthyNodes();
    if (!owner.has_value() && !expected_node_id.empty()) {
        const auto expected = std::find_if(nodes.begin(), nodes.end(), [expected_node_id](const auto& node) {
            return node.node_id == expected_node_id;
        });
        if (expected == nodes.end()
            || !registry_.assignRoom(table_id, expected_node_id, room_ttl_seconds_)) {
            return std::nullopt;
        }
        owner = std::string(expected_node_id);
    }
    if (!owner.has_value()
        || (!expected_node_id.empty() && *owner != expected_node_id)) {
        return std::nullopt;
    }
    for (const auto& node : nodes) {
        if (node.node_id == *owner) {
            return issue(node, table_id, user_id);
        }
    }
    return std::nullopt;
}

std::optional<RouteAssignment> LobbyRouter::issue(const NodeInfo& node,
                                                  TableId table_id,
                                                  storage::UserId user_id) {
    const auto raw_ticket = crypto_.generateSessionToken();
    const auto token_hash = crypto_.hashSessionToken(raw_ticket);
    if (!registry_.storeJoinTicket(token_hash,
                                   JoinTicket{user_id, table_id},
                                   ticket_ttl_seconds_)) {
        return std::nullopt;
    }
    return RouteAssignment{node, raw_ticket};
}

}  // namespace poker::cluster
