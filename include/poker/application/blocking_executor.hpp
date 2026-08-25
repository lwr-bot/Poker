#ifndef POKER_APPLICATION_BLOCKING_EXECUTOR_HPP
#define POKER_APPLICATION_BLOCKING_EXECUTOR_HPP

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace poker::application {

class BlockingExecutor {
public:
    using Task = std::function<void()>;
    using ExceptionHandler = std::function<void(std::exception_ptr)>;

    explicit BlockingExecutor(std::size_t thread_count,
                              std::size_t max_pending = 8'192,
                              ExceptionHandler exception_handler = {});
    ~BlockingExecutor();

    BlockingExecutor(const BlockingExecutor&) = delete;
    BlockingExecutor& operator=(const BlockingExecutor&) = delete;

    bool post(Task task);
    void stop();
    std::size_t pending() const;

private:
    void run();

    std::size_t max_pending_;
    ExceptionHandler exception_handler_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::queue<Task> tasks_;
    std::vector<std::thread> workers_;
    bool stopping_{false};
};

}  // namespace poker::application

#endif

