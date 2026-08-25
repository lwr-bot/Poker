#ifndef POKER_APPLICATION_ROOM_EXECUTOR_HPP
#define POKER_APPLICATION_ROOM_EXECUTOR_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace poker::application {

using TableId = std::uint64_t;

class RoomExecutor {
public:
    using Task = std::function<void()>;
    using ExceptionHandler = std::function<void(std::exception_ptr)>;

    explicit RoomExecutor(std::size_t shard_count,
                          std::size_t max_pending_per_shard = 16'384,
                          ExceptionHandler exception_handler = {});
    ~RoomExecutor();

    RoomExecutor(const RoomExecutor&) = delete;
    RoomExecutor& operator=(const RoomExecutor&) = delete;

    bool post(TableId table_id, Task task);
    void stop();
    std::size_t shardCount() const noexcept;
    std::size_t pendingTasks() const;

private:
    struct Shard {
        std::mutex mutex;
        std::condition_variable ready;
        std::queue<Task> tasks;
        bool stopping{false};
        std::thread worker;
    };

    void run(Shard& shard);
    Shard& shardFor(TableId table_id) noexcept;

    std::size_t max_pending_per_shard_;
    ExceptionHandler exception_handler_;
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace poker::application

#endif

