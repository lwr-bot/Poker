#ifndef POKER_DOMAIN_CARD_HPP
#define POKER_DOMAIN_CARD_HPP

#include <array>
#include <cstdint>
#include <string>

namespace poker::domain {

enum class Suit : std::uint8_t {
    clubs = 0,
    diamonds,
    hearts,
    spades,
};

enum class Rank : std::uint8_t {
    two = 2,
    three,
    four,
    five,
    six,
    seven,
    eight,
    nine,
    ten,
    jack,
    queen,
    king,
    ace,
};

struct Card {
    Rank rank{Rank::two};
    Suit suit{Suit::clubs};

    friend constexpr bool operator==(const Card& lhs, const Card& rhs) noexcept {
        return lhs.rank == rhs.rank && lhs.suit == rhs.suit;
    }

    friend constexpr bool operator!=(const Card& lhs, const Card& rhs) noexcept {
        return !(lhs == rhs);
    }
    
};

constexpr std::uint8_t rankValue(Rank rank) noexcept {
    return static_cast<std::uint8_t>(rank);
}

constexpr std::uint8_t cardIndex(Card card) noexcept {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(card.suit) * 13U
                                     + rankValue(card.rank) - 2U);
}

std::array<Card, 52> orderedDeck();
std::string toString(Card card);

}  // namespace poker::domain

#endif

