#ifndef POKER_APPLICATION_IDEMPOTENCY_CACHE_HPP
#define POKER_APPLICATION_IDEMPOTENCY_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace poker::application {

using UserId = std::uint64_t;
using RequestId = std::uint64_t;
using ClientSequence = std::uint64_t;
using ResponseBytes = std::vector<std::uint8_t>;

enum class RequestStatus : std::uint8_t {
    accepted = 0,
    duplicate,
    in_flight,
    stale_sequence,
    sequence_gap,
    capacity_exceeded,
    invalid_request,
};

struct RequestDecision {
    RequestStatus status{RequestStatus::invalid_request};
    std::optional<ResponseBytes> cached_response;

    explicit operator bool() const noexcept { return status == RequestStatus::accepted; }
};

class IdempotencyCache {
public:
    explicit IdempotencyCache(std::size_t max_requests_per_user = 256);

    RequestDecision begin(UserId user_id, RequestId request_id, ClientSequence sequence);
    bool complete(UserId user_id, RequestId request_id, ResponseBytes response);
    void reset(UserId user_id);
    std::size_t userCount() const;

private:
    struct Entry {
        ClientSequence sequence{0};
        std::optional<ResponseBytes> response;
    };

    struct UserState {
        ClientSequence last_sequence{0};
        std::unordered_map<RequestId, Entry> entries;
        std::deque<RequestId> insertion_order;
    };

    void evictOldest(UserState& state);

    std::size_t max_requests_per_user_;
    mutable std::mutex mutex_;
    std::unordered_map<UserId, UserState> users_;
};

}  // namespace poker::application

#endif
