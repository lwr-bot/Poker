#ifndef POKER_STORAGE_GAME_STORE_VALIDATION_HPP
#define POKER_STORAGE_GAME_STORE_VALIDATION_HPP

#include "poker/storage/game_store.hpp"

#include <array>
#include <limits>
#include <unordered_set>

namespace poker::storage {

inline bool validStoredCard(domain::Card card) noexcept {
    const auto rank = domain::rankValue(card.rank);
    const auto suit = static_cast<std::uint8_t>(card.suit);
    return rank >= 2 && rank <= 14 && suit <= 3;
}

inline bool validHandSettlement(const HandSettlementRecord& settlement) {
    if (settlement.hand_id == 0 || settlement.table_id == 0
        || settlement.hand_number == 0 || settlement.players.size() < 2
        || settlement.players.size() > 6 || settlement.board.size() > 5
        || settlement.total_pot < 0) {
        return false;
    }

    std::array<bool, 52> cards{};
    const auto add_card = [&cards](domain::Card card) {
        if (!validStoredCard(card)) {
            return false;
        }
        const auto index = domain::cardIndex(card);
        if (cards[index]) {
            return false;
        }
        cards[index] = true;
        return true;
    };
    for (const auto card : settlement.board) {
        if (!add_card(card)) {
            return false;
        }
    }

    std::unordered_set<UserId> users;
    std::int64_t total_committed = 0;
    std::int64_t total_winnings = 0;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto& player : settlement.players) {
        if (player.user_id == 0 || player.seat >= 6 || player.start_stack < 0
            || player.committed < 0 || player.committed > player.start_stack
            || player.winnings < 0 || player.end_stack < 0
            || player.hole_cards.size() != 2 || !users.insert(player.user_id).second) {
            return false;
        }
        if (!add_card(player.hole_cards[0]) || !add_card(player.hole_cards[1])) {
            return false;
        }
        const auto after_commitment = player.start_stack - player.committed;
        if (player.winnings > maximum - after_commitment
            || player.end_stack != after_commitment + player.winnings
            || player.committed > maximum - total_committed
            || player.winnings > maximum - total_winnings) {
            return false;
        }
        total_committed += player.committed;
        total_winnings += player.winnings;
    }
    return total_committed == settlement.total_pot
           && total_winnings == settlement.total_pot;
}

inline bool equivalentHandSettlement(const HandSettlementRecord& lhs,
                                     const HandSettlementRecord& rhs) {
    if (lhs.hand_id != rhs.hand_id || lhs.table_id != rhs.table_id
        || lhs.hand_number != rhs.hand_number || lhs.dealer_seat != rhs.dealer_seat
        || lhs.board != rhs.board || lhs.total_pot != rhs.total_pot
        || lhs.players.size() != rhs.players.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.players.size(); ++index) {
        const auto& left = lhs.players[index];
        const auto& right = rhs.players[index];
        if (left.user_id != right.user_id || left.seat != right.seat
            || left.start_stack != right.start_stack || left.committed != right.committed
            || left.winnings != right.winnings || left.end_stack != right.end_stack
            || left.hole_cards != right.hole_cards || left.folded != right.folded) {
            return false;
        }
    }
    return true;
}

}  // namespace poker::storage

#endif
