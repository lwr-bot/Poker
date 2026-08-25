#include "poker/cluster/in_memory_node_registry.hpp"

#include <algorithm>
#include <utility>

namespace poker::cluster {

InMemoryNodeRegistry::InMemoryNodeRegistry(Clock clock) : clock_(std::move(clock)) {}

bool InMemoryNodeRegistry::ping() {
    return true;
}

bool InMemoryNodeRegistry::heartbeat(const NodeInfo& node, std::uint32_t ttl_seconds) {
    if (node.node_id.empty() || node.endpoint.empty() || ttl_seconds == 0) {
        return false;
    }
    const auto now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    nodes_[node.node_id] = {node, expiry(now, ttl_seconds)};
    return true;
}

std::vector<NodeInfo> InMemoryNodeRegistry::healthyNodes() {
    const auto now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NodeInfo> result;
    for (auto iterator = nodes_.begin(); iterator != nodes_.end();) {
        if (iterator->second.expires_at_ms <= now) {
            iterator = nodes_.erase(iterator);
        } else {
            result.push_back(iterator->second.value);
            ++iterator;
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.active_tables != rhs.active_tables) {
            return lhs.active_tables < rhs.active_tables;
        }
        if (lhs.connections != rhs.connections) {
            return lhs.connections < rhs.connections;
        }
        return lhs.node_id < rhs.node_id;
    });
    return result;
}

std::optional<TableId> InMemoryNodeRegistry::allocateTableId() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_table_id_ == 0) {
        return std::nullopt;
    }
    return next_table_id_++;
}

bool InMemoryNodeRegistry::assignRoom(TableId table_id,
                                      std::string_view node_id,
                                      std::uint32_t ttl_seconds) {
    const auto now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    if (table_id == 0 || ttl_seconds == 0 || !nodeIsHealthy(node_id, now)) {
        return false;
    }
    const auto existing = rooms_.find(table_id);
    if (existing != rooms_.end() && existing->second.expires_at_ms > now
        && existing->second.value != node_id) {
        return false;
    }
    const bool new_assignment = existing == rooms_.end() || existing->second.expires_at_ms <= now;
    rooms_[table_id] = {std::string(node_id), expiry(now, ttl_seconds)};
    if (new_assignment) {
        const auto node = nodes_.find(std::string(node_id));
        if (node != nodes_.end()) {
            ++node->second.value.active_tables;
        }
    }
    return true;
}

std::optional<std::string> InMemoryNodeRegistry::roomOwner(TableId table_id) {
    const auto now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto room = rooms_.find(table_id);
    if (room == rooms_.end() || room->second.expires_at_ms <= now
        || !nodeIsHealthy(room->second.value, now)) {
        if (room != rooms_.end()) {
            rooms_.erase(room);
        }
        return std::nullopt;
    }
    return room->second.value;
}

bool InMemoryNodeRegistry::storeJoinTicket(std::string token_hash,
                                           JoinTicket ticket,
                                           std::uint32_t ttl_seconds) {
    if (token_hash.empty() || ticket.user_id == 0 || ticket.table_id == 0 || ttl_seconds == 0) {
        return false;
    }
    const auto now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = tickets_.find(token_hash);
    if (existing != tickets_.end() && existing->second.expires_at_ms > now) {
        return false;
    }
    tickets_[std::move(token_hash)] = {ticket, expiry(now, ttl_seconds)};
    return true;
}

std::optional<JoinTicket> InMemoryNodeRegistry::consumeJoinTicket(std::string_view token_hash) {
    const auto now = clock_();
    std::lock_guard<std::mutex> lock(mutex_);
    const auto ticket = tickets_.find(std::string(token_hash));
    if (ticket == tickets_.end()) {
        return std::nullopt;
    }
    const auto value = ticket->second;
    tickets_.erase(ticket);
    return value.expires_at_ms > now ? std::optional<JoinTicket>{value.value} : std::nullopt;
}

bool InMemoryNodeRegistry::nodeIsHealthy(std::string_view node_id, std::int64_t now) const {
    const auto node = nodes_.find(std::string(node_id));
    return node != nodes_.end() && node->second.expires_at_ms > now;
}

std::int64_t InMemoryNodeRegistry::expiry(std::int64_t now, std::uint32_t ttl_seconds) {
    return now + static_cast<std::int64_t>(ttl_seconds) * 1000;
}

}  // namespace poker::cluster
