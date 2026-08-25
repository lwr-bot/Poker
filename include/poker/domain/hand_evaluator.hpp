#ifndef POKER_DOMAIN_HAND_EVALUATOR_HPP
#define POKER_DOMAIN_HAND_EVALUATOR_HPP

#include "poker/domain/card.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace poker::domain {

enum class HandCategory : std::uint8_t {
    high_card = 0,
    one_pair,
    two_pair,
    three_of_a_kind,
    straight,
    flush,
    full_house,
    four_of_a_kind,
    straight_flush,
};

struct HandValue {
    HandCategory category{HandCategory::high_card};
    std::array<std::uint8_t, 5> tiebreak{};

    friend bool operator==(const HandValue& lhs, const HandValue& rhs) noexcept;
    friend bool operator!=(const HandValue& lhs, const HandValue& rhs) noexcept;
    friend bool operator<(const HandValue& lhs, const HandValue& rhs) noexcept;
    friend bool operator>(const HandValue& lhs, const HandValue& rhs) noexcept;
    friend bool operator<=(const HandValue& lhs, const HandValue& rhs) noexcept;
    friend bool operator>=(const HandValue& lhs, const HandValue& rhs) noexcept;
};

HandValue evaluateFive(const std::array<Card, 5>& cards);
HandValue evaluateBest(const std::vector<Card>& cards);
std::string toString(HandCategory category);

}  // namespace poker::domain

#endif

