#include "poker/application/room_executor.hpp"

#include <stdexcept>
#include <utility>

namespace poker::application {

RoomExecutor::RoomExecutor(std::size_t shard_count,
                           std::size_t max_pending_per_shard,
                           ExceptionHandler exception_handler)
    : max_pending_per_shard_(max_pending_per_shard),
      exception_handler_(std::move(exception_handler)) {
    if (shard_count == 0 || max_pending_per_shard_ == 0) {
        throw std::invalid_argument("room executor requires positive shard and queue sizes");
    }
    shards_.reserve(shard_count);
    for (std::size_t index = 0; index < shard_count; ++index) {
        shards_.push_back(std::make_unique<Shard>());
    }
    for (auto& shard : shards_) {
        shard->worker = std::thread([this, ptr = shard.get()] { run(*ptr); });
    }
}

RoomExecutor::~RoomExecutor() {
    stop();
}

bool RoomExecutor::post(TableId table_id, Task task) {
    if (!task) {
        return false;
    }
    auto& shard = shardFor(table_id);
    {
        std::lock_guard<std::mutex> lock(shard.mutex);
        if (shard.stopping || shard.tasks.size() >= max_pending_per_shard_) {
            return false;
        }
        shard.tasks.push(std::move(task));
    }
    shard.ready.notify_one();
    return true;
}

void RoomExecutor::stop() {
    for (auto& shard : shards_) {
        {
            std::lock_guard<std::mutex> lock(shard->mutex);
            shard->stopping = true;
        }
        shard->ready.notify_all();
    }
    for (auto& shard : shards_) {
        if (shard->worker.joinable()) {
            shard->worker.join();
        }
    }
}

std::size_t RoomExecutor::shardCount() const noexcept {
    return shards_.size();
}

std::size_t RoomExecutor::pendingTasks() const {
    std::size_t total = 0;
    for (const auto& shard : shards_) {
        std::lock_guard<std::mutex> lock(shard->mutex);
        total += shard->tasks.size();
    }
    return total;
}

void RoomExecutor::run(Shard& shard) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(shard.mutex);
            shard.ready.wait(lock, [&shard] { return shard.stopping || !shard.tasks.empty(); });
            if (shard.stopping && shard.tasks.empty()) {
                return;
            }
            task = std::move(shard.tasks.front());
            shard.tasks.pop();
        }

        try {
            task();
        } catch (...) {
            if (exception_handler_) {
                exception_handler_(std::current_exception());
            }
        }
    }
}

RoomExecutor::Shard& RoomExecutor::shardFor(TableId table_id) noexcept {
    return *shards_[static_cast<std::size_t>(table_id % shards_.size())];
}

}  // namespace poker::application
