#include "poker/net/token_bucket.hpp"

#include <algorithm>
#include <stdexcept>

namespace poker::net {

TokenBucket::TokenBucket(double tokens_per_second, std::size_t burst)
    : rate_(tokens_per_second),
      capacity_(static_cast<double>(burst)),
      tokens_(capacity_),
      last_refill_(Clock::now()) {
    if (rate_ <= 0.0 || burst == 0) {
        throw std::invalid_argument("token bucket rate and burst must be positive");
    }
}

bool TokenBucket::allow(Clock::time_point now) noexcept {
    if (now > last_refill_) {
        const auto elapsed = std::chrono::duration<double>(now - last_refill_).count();
        tokens_ = std::min(capacity_, tokens_ + elapsed * rate_);
        last_refill_ = now;
    }
    if (tokens_ < 1.0) {
        return false;
    }
    tokens_ -= 1.0;
    return true;
}

double TokenBucket::available() const noexcept {
    return tokens_;
}

}  // namespace poker::net
