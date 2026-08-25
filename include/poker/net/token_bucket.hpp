#ifndef POKER_NET_TOKEN_BUCKET_HPP
#define POKER_NET_TOKEN_BUCKET_HPP

#include <chrono>
#include <cstddef>

namespace poker::net {

class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    TokenBucket(double tokens_per_second, std::size_t burst);
    bool allow(Clock::time_point now = Clock::now()) noexcept;
    double available() const noexcept;

private:
    double rate_;
    double capacity_;
    double tokens_;
    Clock::time_point last_refill_;
};

}  // namespace poker::net

#endif
