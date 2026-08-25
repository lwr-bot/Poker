#ifndef POKER_CLUSTER_NODE_FAILURE_REAPER_HPP
#define POKER_CLUSTER_NODE_FAILURE_REAPER_HPP

#include "poker/cluster/node_registry.hpp"
#include "poker/storage/game_store.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace poker::cluster {

struct ReapResult {
    std::vector<TableId> refunded_tables;
    std::vector<TableId> failed_tables;
};

class NodeFailureReaper {
public:
    using Clock = std::function<std::int64_t()>;

    NodeFailureReaper(NodeRegistry& registry,
                      storage::GameStore& game_store,
                      std::chrono::milliseconds grace_period = std::chrono::seconds(15),
                      Clock clock = {});

    ReapResult sweep();

private:
    static std::int64_t systemNowMs();

    NodeRegistry& registry_;
    storage::GameStore& game_store_;
    std::int64_t grace_period_ms_;
    Clock clock_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::int64_t> missing_since_;
};

}  // namespace poker::cluster

#endif
