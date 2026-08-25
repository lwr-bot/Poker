#include "poker/observability/metrics.hpp"

#include <sstream>

namespace poker::observability {
namespace {

void type(std::ostringstream& output, const char* name, const char* metric_type) {
    output << "# TYPE " << name << ' ' << metric_type << '\n';
}

}  // namespace

void MetricsRegistry::connectionOpened() noexcept {
    active_connections_.fetch_add(1, std::memory_order_relaxed);
    opened_connections_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::connectionClosed() noexcept {
    auto current = active_connections_.load(std::memory_order_relaxed);
    while (current > 0
           && !active_connections_.compare_exchange_weak(
               current, current - 1, std::memory_order_relaxed)) {
    }
}

void MetricsRegistry::requestReceived() noexcept {
    requests_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::responseSent() noexcept {
    responses_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::invalidFrame() noexcept {
    invalid_frames_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::rejectedRequest() noexcept {
    rejected_requests_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::actionAccepted() noexcept {
    accepted_actions_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::actionRejected() noexcept {
    rejected_actions_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::storageFailure() noexcept {
    storage_failures_.fetch_add(1, std::memory_order_relaxed);
}

void MetricsRegistry::setTables(std::size_t value) noexcept {
    tables_.store(static_cast<std::uint64_t>(value), std::memory_order_relaxed);
}

void MetricsRegistry::observeRequestLatency(std::chrono::microseconds latency) noexcept {
    const auto value = latency.count() < 0 ? std::uint64_t{0}
                                           : static_cast<std::uint64_t>(latency.count());
    std::size_t bucket = 0;
    while (bucket < latency_bounds_us_.size() && value > latency_bounds_us_[bucket]) {
        ++bucket;
    }
    latency_buckets_[bucket].fetch_add(1, std::memory_order_relaxed);
    latency_count_.fetch_add(1, std::memory_order_relaxed);
    latency_sum_us_.fetch_add(value, std::memory_order_relaxed);
}

std::uint64_t MetricsRegistry::activeConnections() const noexcept {
    return active_connections_.load(std::memory_order_relaxed);
}

std::string MetricsRegistry::renderPrometheus() const {
    std::ostringstream output;
    type(output, "poker_connections", "gauge");
    output << "poker_connections " << active_connections_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_connections_opened_total", "counter");
    output << "poker_connections_opened_total "
           << opened_connections_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_requests_total", "counter");
    output << "poker_requests_total " << requests_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_responses_total", "counter");
    output << "poker_responses_total " << responses_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_invalid_frames_total", "counter");
    output << "poker_invalid_frames_total " << invalid_frames_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_rejected_requests_total", "counter");
    output << "poker_rejected_requests_total "
           << rejected_requests_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_actions_total", "counter");
    output << "poker_actions_total{result=\"accepted\"} "
           << accepted_actions_.load(std::memory_order_relaxed) << '\n';
    output << "poker_actions_total{result=\"rejected\"} "
           << rejected_actions_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_storage_failures_total", "counter");
    output << "poker_storage_failures_total "
           << storage_failures_.load(std::memory_order_relaxed) << '\n';
    type(output, "poker_tables", "gauge");
    output << "poker_tables " << tables_.load(std::memory_order_relaxed) << '\n';

    type(output, "poker_request_latency_seconds", "histogram");
    std::uint64_t cumulative = 0;
    for (std::size_t index = 0; index < latency_bounds_us_.size(); ++index) {
        cumulative += latency_buckets_[index].load(std::memory_order_relaxed);
        output << "poker_request_latency_seconds_bucket{le=\""
               << static_cast<double>(latency_bounds_us_[index]) / 1'000'000.0
               << "\"} " << cumulative << '\n';
    }
    cumulative += latency_buckets_.back().load(std::memory_order_relaxed);
    output << "poker_request_latency_seconds_bucket{le=\"+Inf\"} " << cumulative << '\n';
    output << "poker_request_latency_seconds_count "
           << latency_count_.load(std::memory_order_relaxed) << '\n';
    output << "poker_request_latency_seconds_sum "
           << static_cast<double>(latency_sum_us_.load(std::memory_order_relaxed)) / 1'000'000.0
           << '\n';
    return output.str();
}

}  // namespace poker::observability
