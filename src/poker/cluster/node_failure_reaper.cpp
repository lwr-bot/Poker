#include "poker/cluster/node_failure_reaper.hpp"

#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace poker::cluster {

NodeFailureReaper::NodeFailureReaper(NodeRegistry& registry,
                                     storage::GameStore& game_store,
                                     std::chrono::milliseconds grace_period,
                                     Clock clock)
    : registry_(registry),
      game_store_(game_store),
      grace_period_ms_(grace_period.count()),
      clock_(clock ? std::move(clock) : Clock{systemNowMs}) {
    if (grace_period_ms_ <= 0) {
        throw std::invalid_argument("node failure grace period must be positive");
    }
}

ReapResult NodeFailureReaper::sweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    ReapResult result;
    if (!registry_.ping()) {
        return result;
    }
    const auto now = clock_();
    const auto nodes = registry_.healthyNodes();
    std::unordered_set<std::string> healthy;
    for (const auto& node : nodes) {
        healthy.insert(node.node_id);
        missing_since_.erase(node.node_id);
    }

    const auto tables = game_store_.listOpenTables(200);
    if (!tables) {
        return result;
    }
    for (const auto& table : *tables.value) {
        if (healthy.count(table.node_id) != 0) {
            continue;
        }
        const auto [missing, inserted] = missing_since_.emplace(table.node_id, now);
        if (inserted || now - missing->second < grace_period_ms_) {
            continue;
        }
        const auto refunded = game_store_.abortTableAndRefund(table.table_id);
        if (refunded == storage::StorageError::ok) {
            result.refunded_tables.push_back(table.table_id);
        } else {
            result.failed_tables.push_back(table.table_id);
        }
    }
    return result;
}

std::int64_t NodeFailureReaper::systemNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace poker::cluster
