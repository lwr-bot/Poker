#include "poker/domain/card.hpp"

#include <stdexcept>

namespace poker::domain {

std::array<Card, 52> orderedDeck() {
    std::array<Card, 52> cards{};
    std::size_t index = 0;
    for (std::uint8_t suit = 0; suit < 4; ++suit) {
        for (std::uint8_t rank = 2; rank <= 14; ++rank) {
            cards[index++] = Card{static_cast<Rank>(rank), static_cast<Suit>(suit)};
        }
    }
    return cards;
}

std::string toString(Card card) {
    static constexpr const char* ranks[] = {
        "", "", "2", "3", "4", "5", "6", "7", "8", "9", "T", "J", "Q", "K", "A"};
    static constexpr const char* suits[] = {"c", "d", "h", "s"};

    const auto rank = rankValue(card.rank);
    const auto suit = static_cast<std::uint8_t>(card.suit);
    if (rank < 2 || rank > 14 || suit > 3) {
        throw std::invalid_argument("invalid card");
    }
    return std::string(ranks[rank]) + suits[suit];
}

}  // namespace poker::domain

