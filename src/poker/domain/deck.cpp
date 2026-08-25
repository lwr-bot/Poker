#include "poker/domain/deck.hpp"

#include <limits>
#include <random>
#include <stdexcept>

namespace poker::domain {

std::uint64_t SystemRandomSource::uniform(std::uint64_t exclusive_upper_bound) {
    if (exclusive_upper_bound == 0) {
        throw std::invalid_argument("random upper bound must be positive");
    }

    static thread_local std::random_device device;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto limit = maximum - (maximum % exclusive_upper_bound);
    std::uint64_t value = 0;
    do {
        value = (static_cast<std::uint64_t>(device()) << 32U) ^ device();
    } while (value >= limit);
    return value % exclusive_upper_bound;
}

DeterministicRandomSource::DeterministicRandomSource(std::uint64_t seed)
    : state_(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

std::uint64_t DeterministicRandomSource::uniform(std::uint64_t exclusive_upper_bound) {
    if (exclusive_upper_bound == 0) {
        throw std::invalid_argument("random upper bound must be positive");
    }
    state_ ^= state_ >> 12U;
    state_ ^= state_ << 25U;
    state_ ^= state_ >> 27U;
    return (state_ * 2685821657736338717ULL) % exclusive_upper_bound;
}

Deck::Deck(RandomSource& random) {
    const auto ordered = orderedDeck();
    cards_.assign(ordered.begin(), ordered.end());
    for (std::size_t index = cards_.size(); index > 1; --index) {
        const auto chosen = static_cast<std::size_t>(random.uniform(index));
        std::swap(cards_[index - 1], cards_[chosen]);
    }
}

Deck::Deck(std::vector<Card> cards) : cards_(std::move(cards)) {
    if (cards_.size() != 52) {
        throw std::invalid_argument("a poker deck must contain 52 cards");
    }

    std::array<bool, 52> seen{};
    for (const auto card : cards_) {
        const auto index = cardIndex(card);
        if (index >= seen.size() || seen[index]) {
            throw std::invalid_argument("deck contains an invalid or duplicate card");
        }
        seen[index] = true;
    }
}

Card Deck::draw() {
    if (next_ >= cards_.size()) {
        throw std::out_of_range("deck is empty");
    }
    return cards_[next_++];
}

void Deck::burn() {
    static_cast<void>(draw());
}

std::size_t Deck::remaining() const noexcept {
    return cards_.size() - next_;
}

}  // namespace poker::domain

