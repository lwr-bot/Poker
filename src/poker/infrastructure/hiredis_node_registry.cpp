#include "poker/cluster/hiredis_node_registry.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace poker::cluster {
namespace {

std::string nodeKey(std::string_view node_id) {
    return "poker:node:" + std::string(node_id);
}

std::string roomKey(TableId table_id) {
    return "poker:room:" + std::to_string(table_id);
}

std::string ticketKey(std::string_view token_hash) {
    return "poker:ticket:" + std::string(token_hash);
}

}  // namespace

HiredisNodeRegistry::HiredisNodeRegistry(config::RedisConfig config)
    : config_(std::move(config)) {
    std::lock_guard<std::mutex> lock(mutex_);
    connectLocked();
}

HiredisNodeRegistry::~HiredisNodeRegistry() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (context_ != nullptr) {
        redisFree(context_);
    }
}

bool HiredisNodeRegistry::ping() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto reply = commandLocked("PING");
    return reply && reply->type == REDIS_REPLY_STATUS;
}

bool HiredisNodeRegistry::heartbeat(const NodeInfo& node, std::uint32_t ttl_seconds) {
    if (node.node_id.empty() || node.endpoint.empty() || ttl_seconds == 0) {
        return false;
    }
    const auto key = nodeKey(node.node_id);
    const auto value = node.endpoint + "|" + std::to_string(node.active_tables) + "|"
                       + std::to_string(node.connections);
    const auto score = node.active_tables * 1'000'000ULL + node.connections;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto stored = commandLocked("SET %b %b EX %u",
                                      key.data(), key.size(),
                                      value.data(), value.size(),
                                      ttl_seconds);
    const auto indexed = commandLocked("ZADD poker:nodes:load %llu %b",
                                       static_cast<unsigned long long>(score),
                                       node.node_id.data(), node.node_id.size());
    return stored && stored->type == REDIS_REPLY_STATUS && indexed
           && (indexed->type == REDIS_REPLY_INTEGER || indexed->type == REDIS_REPLY_STATUS);
}

std::vector<NodeInfo> HiredisNodeRegistry::healthyNodes() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto ids = commandLocked("ZRANGE poker:nodes:load 0 -1");
    std::vector<NodeInfo> result;
    if (!ids || ids->type != REDIS_REPLY_ARRAY) {
        return result;
    }
    for (std::size_t index = 0; index < ids->elements; ++index) {
        const auto* item = ids->element[index];
        if (item == nullptr || item->type != REDIS_REPLY_STRING) {
            continue;
        }
        const std::string node_id(item->str, item->len);
        const auto key = nodeKey(node_id);
        const auto value = commandLocked("GET %b", key.data(), key.size());
        if (!value || value->type == REDIS_REPLY_NIL) {
            commandLocked("ZREM poker:nodes:load %b", node_id.data(), node_id.size());
            continue;
        }
        if (value->type != REDIS_REPLY_STRING) {
            continue;
        }
        const auto parsed = parseNode(node_id, std::string_view(value->str, value->len));
        if (parsed.has_value()) {
            result.push_back(*parsed);
        }
    };
    return result;
}

std::optional<TableId> HiredisNodeRegistry::allocateTableId() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto reply = commandLocked("INCR poker:ids:table");
    if (!reply || reply->type != REDIS_REPLY_INTEGER || reply->integer <= 0
        || static_cast<unsigned long long>(reply->integer)
               > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<TableId>(reply->integer);
}

bool HiredisNodeRegistry::assignRoom(TableId table_id,
                                     std::string_view node_id,
                                     std::uint32_t ttl_seconds) {
    if (table_id == 0 || node_id.empty() || ttl_seconds == 0) {
        return false;
    }
    static constexpr const char* script =
        "local current=redis.call('GET',KEYS[1]); "
        "if not current then redis.call('SET',KEYS[1],ARGV[1],'EX',ARGV[2]); "
        "redis.call('ZINCRBY','poker:nodes:load',1000000,ARGV[1]); return 1; end; "
        "if current==ARGV[1] then redis.call('SET',KEYS[1],ARGV[1],'EX',ARGV[2]); "
        "return 1; end; return 0";
    const auto key = roomKey(table_id);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto reply = commandLocked("EVAL %s 1 %b %b %u",
                                     script,
                                     key.data(), key.size(),
                                     node_id.data(), node_id.size(),
                                     ttl_seconds);
    return reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
}

std::optional<std::string> HiredisNodeRegistry::roomOwner(TableId table_id) {
    const auto key = roomKey(table_id);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto reply = commandLocked("GET %b", key.data(), key.size());
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        return std::nullopt;
    }
    return std::string(reply->str, reply->len);
}

bool HiredisNodeRegistry::storeJoinTicket(std::string token_hash,
                                          JoinTicket ticket,
                                          std::uint32_t ttl_seconds) {
    if (token_hash.empty() || ticket.user_id == 0 || ticket.table_id == 0 || ttl_seconds == 0) {
        return false;
    }
    const auto key = ticketKey(token_hash);
    const auto value = std::to_string(ticket.user_id) + ":" + std::to_string(ticket.table_id);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto reply = commandLocked("SET %b %b EX %u NX",
                                     key.data(), key.size(),
                                     value.data(), value.size(),
                                     ttl_seconds);
    return reply && reply->type == REDIS_REPLY_STATUS;
}

std::optional<JoinTicket> HiredisNodeRegistry::consumeJoinTicket(std::string_view token_hash) {
    const auto key = ticketKey(token_hash);
    std::lock_guard<std::mutex> lock(mutex_);
    const auto reply = commandLocked("GETDEL %b", key.data(), key.size());
    if (!reply || reply->type != REDIS_REPLY_STRING) {
        return std::nullopt;
    }
    const std::string value(reply->str, reply->len);
    const auto separator = value.find(':');
    if (separator == std::string::npos) {
        return std::nullopt;
    }
    try {
        return JoinTicket{static_cast<storage::UserId>(std::stoull(value.substr(0, separator))),
                          static_cast<TableId>(std::stoull(value.substr(separator + 1)))};
    } catch (...) {
        return std::nullopt;
    }
}

bool HiredisNodeRegistry::connectLocked() {
    if (context_ != nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(config_.connect_timeout_ms / 1000);
    timeout.tv_usec = static_cast<long>((config_.connect_timeout_ms % 1000) * 1000);
    context_ = redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout);
    if (context_ == nullptr || context_->err != 0) {
        if (context_ != nullptr) {
            redisFree(context_);
            context_ = nullptr;
        }
        return false;
    }
    if (!config_.password.empty()) {
        const auto auth = commandLocked("AUTH %b", config_.password.data(), config_.password.size());
        if (!auth || auth->type == REDIS_REPLY_ERROR) {
            if (context_ != nullptr) {
                redisFree(context_);
                context_ = nullptr;
            }
            return false;
        }
    }
    if (config_.database != 0) {
        const auto selected = commandLocked("SELECT %d", config_.database);
        if (!selected || selected->type == REDIS_REPLY_ERROR) {
            if (context_ != nullptr) {
                redisFree(context_);
                context_ = nullptr;
            }
            return false;
        }
    }
    return true;
}

HiredisNodeRegistry::Reply HiredisNodeRegistry::commandLocked(const char* format, ...) {
    if (context_ == nullptr && !connectLocked()) {
        return {nullptr, &freeReplyObject};
    }
    va_list arguments;
    va_start(arguments, format);
    auto* raw = static_cast<redisReply*>(redisvCommand(context_, format, arguments));
    va_end(arguments);
    if (raw == nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }
    return {raw, &freeReplyObject};
}

std::optional<NodeInfo> HiredisNodeRegistry::parseNode(std::string_view node_id,
                                                       std::string_view value) {
    const auto first = value.rfind('|');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const auto second = value.rfind('|', first - 1);
    if (second == std::string_view::npos) {
        return std::nullopt;
    }
    try {
        NodeInfo node;
        node.node_id = std::string(node_id);
        node.endpoint = std::string(value.substr(0, second));
        node.active_tables = std::stoull(std::string(value.substr(second + 1, first - second - 1)));
        node.connections = std::stoull(std::string(value.substr(first + 1)));
        return node;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace poker::cluster
