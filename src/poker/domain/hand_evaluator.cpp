#include "poker/domain/hand_evaluator.hpp"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace poker::domain {
namespace {

std::uint8_t straightHigh(const std::array<int, 15>& counts) {
    int consecutive = 0;
    for (int rank = 14; rank >= 2; --rank) {
        if (counts[static_cast<std::size_t>(rank)] > 0) {
            ++consecutive;
            if (consecutive == 5) {
                return static_cast<std::uint8_t>(rank + 4);
            }
        } else {
            consecutive = 0;
        }
    }

    if (counts[14] > 0 && counts[2] > 0 && counts[3] > 0 && counts[4] > 0 && counts[5] > 0) {
        return 5;
    }
    return 0;
}

std::array<std::uint8_t, 5> descendingRanks(const std::array<int, 15>& counts) {
    std::array<std::uint8_t, 5> result{};
    std::size_t output = 0;
    for (int rank = 14; rank >= 2; --rank) {
        for (int count = 0; count < counts[static_cast<std::size_t>(rank)]; ++count) {
            result[output++] = static_cast<std::uint8_t>(rank);
        }
    }
    return result;
}

}  // namespace

bool operator==(const HandValue& lhs, const HandValue& rhs) noexcept {
    return lhs.category == rhs.category && lhs.tiebreak == rhs.tiebreak;
}

bool operator!=(const HandValue& lhs, const HandValue& rhs) noexcept {
    return !(lhs == rhs);
}

bool operator<(const HandValue& lhs, const HandValue& rhs) noexcept {
    return std::tie(lhs.category, lhs.tiebreak) < std::tie(rhs.category, rhs.tiebreak);
}

bool operator>(const HandValue& lhs, const HandValue& rhs) noexcept {
    return rhs < lhs;
}

bool operator<=(const HandValue& lhs, const HandValue& rhs) noexcept {
    return !(rhs < lhs);
}

bool operator>=(const HandValue& lhs, const HandValue& rhs) noexcept {
    return !(lhs < rhs);
}

HandValue evaluateFive(const std::array<Card, 5>& cards) {
    std::array<int, 15> counts{};
    std::array<int, 4> suit_counts{};
    std::array<bool, 52> seen{};

    for (const auto card : cards) {
        const auto index = cardIndex(card);
        if (index >= seen.size() || seen[index]) {
            throw std::invalid_argument("hand contains an invalid or duplicate card");
        }
        seen[index] = true;
        ++counts[rankValue(card.rank)];
        ++suit_counts[static_cast<std::size_t>(card.suit)];
    }

    const bool flush = std::any_of(suit_counts.begin(), suit_counts.end(), [](int count) { return count == 5; });
    const auto straight = straightHigh(counts);
    if (flush && straight != 0) {
        return {HandCategory::straight_flush, {straight, 0, 0, 0, 0}};
    }

    std::uint8_t four = 0;
    std::uint8_t three = 0;
    std::array<std::uint8_t, 2> pairs{};
    std::size_t pair_count = 0;
    for (int rank = 14; rank >= 2; --rank) {
        const int count = counts[static_cast<std::size_t>(rank)];
        if (count == 4) {
            four = static_cast<std::uint8_t>(rank);
        } else if (count == 3) {
            three = static_cast<std::uint8_t>(rank);
        } else if (count == 2 && pair_count < pairs.size()) {
            pairs[pair_count++] = static_cast<std::uint8_t>(rank);
        }
    }

    if (four != 0) {
        std::uint8_t kicker = 0;
        for (int rank = 14; rank >= 2; --rank) {
            if (counts[static_cast<std::size_t>(rank)] == 1) {
                kicker = static_cast<std::uint8_t>(rank);
                break;
            }
        }
        return {HandCategory::four_of_a_kind, {four, kicker, 0, 0, 0}};
    }

    if (three != 0 && pair_count > 0) {
        return {HandCategory::full_house, {three, pairs[0], 0, 0, 0}};
    }
    if (flush) {
        return {HandCategory::flush, descendingRanks(counts)};
    }
    if (straight != 0) {
        return {HandCategory::straight, {straight, 0, 0, 0, 0}};
    }
    if (three != 0) {
        std::array<std::uint8_t, 5> tiebreak{three, 0, 0, 0, 0};
        std::size_t output = 1;
        for (int rank = 14; rank >= 2 && output < 3; --rank) {
            if (counts[static_cast<std::size_t>(rank)] == 1) {
                tiebreak[output++] = static_cast<std::uint8_t>(rank);
            }
        }
        return {HandCategory::three_of_a_kind, tiebreak};
    }
    if (pair_count >= 2) {
        std::uint8_t kicker = 0;
        for (int rank = 14; rank >= 2; --rank) {
            if (counts[static_cast<std::size_t>(rank)] == 1) {
                kicker = static_cast<std::uint8_t>(rank);
                break;
            }
        }
        return {HandCategory::two_pair, {pairs[0], pairs[1], kicker, 0, 0}};
    }
    if (pair_count == 1) {
        std::array<std::uint8_t, 5> tiebreak{pairs[0], 0, 0, 0, 0};
        std::size_t output = 1;
        for (int rank = 14; rank >= 2 && output < 4; --rank) {
            if (counts[static_cast<std::size_t>(rank)] == 1) {
                tiebreak[output++] = static_cast<std::uint8_t>(rank);
            }
        }
        return {HandCategory::one_pair, tiebreak};
    }
    return {HandCategory::high_card, descendingRanks(counts)};
}

HandValue evaluateBest(const std::vector<Card>& cards) {
    if (cards.size() < 5 || cards.size() > 7) {
        throw std::invalid_argument("best-hand evaluation requires five to seven cards");
    }

    std::array<bool, 52> seen{};
    for (const auto card : cards) {
        const auto index = cardIndex(card);
        if (index >= seen.size() || seen[index]) {
            throw std::invalid_argument("hand contains an invalid or duplicate card");
        }
        seen[index] = true;
    }

    HandValue best{};
    bool initialized = false;
    const auto count = cards.size();
    for (std::size_t a = 0; a + 4 < count; ++a) {
        for (std::size_t b = a + 1; b + 3 < count; ++b) {
            for (std::size_t c = b + 1; c + 2 < count; ++c) {
                for (std::size_t d = c + 1; d + 1 < count; ++d) {
                    for (std::size_t e = d + 1; e < count; ++e) {
                        const auto value = evaluateFive({cards[a], cards[b], cards[c], cards[d], cards[e]});
                        if (!initialized || best < value) {
                            best = value;
                            initialized = true;
                        }
                    }
                }
            }
        }
    }
    return best;
}

std::string toString(HandCategory category) {
    switch (category) {
    case HandCategory::high_card: return "high card";
    case HandCategory::one_pair: return "one pair";
    case HandCategory::two_pair: return "two pair";
    case HandCategory::three_of_a_kind: return "three of a kind";
    case HandCategory::straight: return "straight";
    case HandCategory::flush: return "flush";
    case HandCategory::full_house: return "full house";
    case HandCategory::four_of_a_kind: return "four of a kind";
    case HandCategory::straight_flush: return "straight flush";
    }
    return "unknown";
}

}  // namespace poker::domain
