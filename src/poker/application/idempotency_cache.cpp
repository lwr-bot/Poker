#include "poker/application/idempotency_cache.hpp"

#include <stdexcept>
#include <utility>

namespace poker::application {

IdempotencyCache::IdempotencyCache(std::size_t max_requests_per_user)
    : max_requests_per_user_(max_requests_per_user) {
    if (max_requests_per_user_ == 0) {
        throw std::invalid_argument("idempotency cache must retain at least one request");
    }
}

RequestDecision IdempotencyCache::begin(UserId user_id,
                                        RequestId request_id,
                                        ClientSequence sequence) {
    if (user_id == 0 || request_id == 0 || sequence == 0) {
        return {RequestStatus::invalid_request, std::nullopt};
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto& state = users_[user_id];
    const auto existing = state.entries.find(request_id);
    if (existing != state.entries.end()) {
        if (existing->second.response.has_value()) {
            return {RequestStatus::duplicate, existing->second.response};
        }
        return {RequestStatus::in_flight, std::nullopt};
    }
    if (sequence <= state.last_sequence) {
        return {RequestStatus::stale_sequence, std::nullopt};
    }
    if (sequence != state.last_sequence + 1) {
        return {RequestStatus::sequence_gap, std::nullopt};
    }

    evictOldest(state);
    if (state.entries.size() >= max_requests_per_user_) {
        return {RequestStatus::capacity_exceeded, std::nullopt};
    }

    state.last_sequence = sequence;
    state.entries.emplace(request_id, Entry{sequence, std::nullopt});
    state.insertion_order.push_back(request_id);
    return {RequestStatus::accepted, std::nullopt};
}

bool IdempotencyCache::complete(UserId user_id, RequestId request_id, ResponseBytes response) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto user = users_.find(user_id);
    if (user == users_.end()) {
        return false;
    }
    const auto entry = user->second.entries.find(request_id);
    if (entry == user->second.entries.end()) {
        return false;
    }
    entry->second.response = std::move(response);
    return true;
}

void IdempotencyCache::reset(UserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    users_.erase(user_id);
}

std::size_t IdempotencyCache::userCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return users_.size();
}

void IdempotencyCache::evictOldest(UserState& state) {
    auto request = state.insertion_order.begin();
    while (state.entries.size() >= max_requests_per_user_
           && request != state.insertion_order.end()) {
        const auto entry = state.entries.find(*request);
        if (entry == state.entries.end()) {
            request = state.insertion_order.erase(request);
            continue;
        }
        if (entry->second.response.has_value()) {
            state.entries.erase(entry);
            request = state.insertion_order.erase(request);
            continue;
        }
        ++request;
    }
}

}  // namespace poker::application
