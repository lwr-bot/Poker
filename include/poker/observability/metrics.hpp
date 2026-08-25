#ifndef POKER_OBSERVABILITY_METRICS_HPP
#define POKER_OBSERVABILITY_METRICS_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace poker::observability {

class MetricsRegistry {
public:
    void connectionOpened() noexcept;
    void connectionClosed() noexcept;
    void requestReceived() noexcept;
    void responseSent() noexcept;
    void invalidFrame() noexcept;
    void rejectedRequest() noexcept;
    void actionAccepted() noexcept;
    void actionRejected() noexcept;
    void storageFailure() noexcept;
    void setTables(std::size_t value) noexcept;
    void observeRequestLatency(std::chrono::microseconds latency) noexcept;
    std::uint64_t activeConnections() const noexcept;

    std::string renderPrometheus() const;

private:
    static constexpr std::array<std::uint64_t, 10> latency_bounds_us_{
        1'000, 5'000, 10'000, 25'000, 50'000,
        100'000, 250'000, 500'000, 1'000'000, 5'000'000,
    };

    std::atomic<std::uint64_t> active_connections_{0};
    std::atomic<std::uint64_t> opened_connections_{0};
    std::atomic<std::uint64_t> requests_{0};
    std::atomic<std::uint64_t> responses_{0};
    std::atomic<std::uint64_t> invalid_frames_{0};
    std::atomic<std::uint64_t> rejected_requests_{0};
    std::atomic<std::uint64_t> accepted_actions_{0};
    std::atomic<std::uint64_t> rejected_actions_{0};
    std::atomic<std::uint64_t> storage_failures_{0};
    std::atomic<std::uint64_t> tables_{0};
    std::array<std::atomic<std::uint64_t>, latency_bounds_us_.size() + 1> latency_buckets_{};
    std::atomic<std::uint64_t> latency_count_{0};
    std::atomic<std::uint64_t> latency_sum_us_{0};
};

}  // namespace poker::observability

#endif
