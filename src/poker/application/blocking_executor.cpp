#include "poker/application/blocking_executor.hpp"

#include <stdexcept>
#include <utility>

namespace poker::application {

BlockingExecutor::BlockingExecutor(std::size_t thread_count,
                                   std::size_t max_pending,
                                   ExceptionHandler exception_handler)
    : max_pending_(max_pending), exception_handler_(std::move(exception_handler)) {
    if (thread_count == 0 || max_pending_ == 0) {
        throw std::invalid_argument("blocking executor requires positive thread and queue sizes");
    }
    workers_.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        workers_.emplace_back([this] { run(); });
    }
}

BlockingExecutor::~BlockingExecutor() {
    stop();
}

bool BlockingExecutor::post(Task task) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || tasks_.size() >= max_pending_) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    ready_.notify_one();
    return true;
}

void BlockingExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t BlockingExecutor::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void BlockingExecutor::run() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (stopping_ && tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
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

}  // namespace poker::application
