#ifndef POKER_DOMAIN_DECK_HPP
#define POKER_DOMAIN_DECK_HPP

#include "poker/domain/card.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace poker::domain {

class RandomSource {
public:
    virtual ~RandomSource() = default;
    virtual std::uint64_t uniform(std::uint64_t exclusive_upper_bound) = 0;
};

class SystemRandomSource final : public RandomSource {
public:
    std::uint64_t uniform(std::uint64_t exclusive_upper_bound) override;
};

class DeterministicRandomSource final : public RandomSource {
public:
    explicit DeterministicRandomSource(std::uint64_t seed);
    std::uint64_t uniform(std::uint64_t exclusive_upper_bound) override;

private:
    std::uint64_t state_;
};

class Deck {
public:
    explicit Deck(RandomSource& random);
    explicit Deck(std::vector<Card> cards);

    Card draw();
    void burn();
    std::size_t remaining() const noexcept;

private:
    std::vector<Card> cards_;
    std::size_t next_{0};
};

}  // namespace poker::domain

#endif

