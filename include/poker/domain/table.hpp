#ifndef POKER_DOMAIN_TABLE_HPP
#define POKER_DOMAIN_TABLE_HPP

#include "poker/domain/card.hpp"
#include "poker/domain/deck.hpp"
#include "poker/domain/hand_evaluator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace poker::domain {

using PlayerId = std::uint64_t;
using Chips = std::int64_t;

enum class Street : std::uint8_t {
    waiting = 0,
    preflop,
    flop,
    turn,
    river,
    showdown,
    settled,
};

enum class PlayerStatus : std::uint8_t {
    waiting = 0,
    active,
    folded,
    all_in,
    disconnected,
};

enum class ActionType : std::uint8_t {
    fold = 0,
    check,
    call,
    bet,
    raise,
    all_in,
};

enum class TableError : std::uint8_t {
    ok = 0,
    invalid_configuration,
    seat_out_of_range,
    seat_occupied,
    player_already_seated,
    player_not_found,
    invalid_buy_in,
    hand_in_progress,
    not_enough_players,
    player_not_ready,
    not_players_turn,
    player_cannot_act,
    action_not_allowed,
    invalid_amount,
    minimum_raise_not_met,
    service_unavailable,
};

struct TableConfig {
    std::size_t min_players{2};
    std::size_t max_players{6};
    Chips small_blind{50};
    Chips big_blind{100};
    Chips min_buy_in{2'000};
    Chips max_buy_in{20'000};
};

struct PlayerView {
    PlayerId id{0};
    std::size_t seat{0};
    Chips stack{0};
    Chips street_commitment{0};
    Chips hand_commitment{0};
    PlayerStatus status{PlayerStatus::waiting};
    bool ready{false};
    bool connected{true};
    std::vector<Card> hole_cards;
};

struct TableSnapshot {
    Street street{Street::waiting};
    std::uint64_t hand_id{0};
    std::uint64_t server_sequence{0};
    std::optional<PlayerId> dealer;
    std::optional<PlayerId> acting_player;
    Chips current_bet{0};
    Chips minimum_raise{0};
    Chips pot{0};
    std::vector<Card> board;
    std::vector<PlayerView> players;
};

struct ActionCommand {
    PlayerId player_id{0};
    ActionType type{ActionType::fold};
    // For bet/raise this is the player's target total commitment on this street.
    Chips amount{0};
};

struct PotAward {
    Chips pot_size{0};
    std::vector<PlayerId> winners;
    Chips equal_share{0};
    Chips odd_chips{0};
};

struct ActionResult {
    TableError error{TableError::ok};
    std::string message;
    std::uint64_t server_sequence{0};
    std::vector<PotAward> awards;
    Street action_street{Street::waiting};

    explicit operator bool() const noexcept { return error == TableError::ok; }
};

class Table {
public:
    explicit Table(TableConfig config = {});

    TableError seatPlayer(PlayerId id, std::size_t seat, Chips buy_in);
    TableError removePlayer(PlayerId id);
    TableError setReady(PlayerId id, bool ready);
    TableError setConnected(PlayerId id, bool connected);

    TableError startHand(RandomSource& random);
    TableError startHandWithDeck(std::vector<Card> cards);
    TableError abortHand();
    ActionResult act(const ActionCommand& command);

    TableSnapshot snapshot(std::optional<PlayerId> viewer = std::nullopt) const;
    TableSnapshot auditSnapshot() const;
    const std::vector<PotAward>& lastAwards() const noexcept;
    Chips totalChips() const noexcept;

private:
    struct Player {
        PlayerId id{0};
        std::size_t seat{0};
        Chips stack{0};
        Chips street_commitment{0};
        Chips hand_commitment{0};
        PlayerStatus status{PlayerStatus::waiting};
        bool ready{false};
        bool connected{true};
        std::array<Card, 2> hole{};
        bool has_hole{false};
    };

    TableError validateConfig() const noexcept;
    TableError startHand(std::unique_ptr<Deck> deck);
    Player* findPlayer(PlayerId id) noexcept;
    const Player* findPlayer(PlayerId id) const noexcept;
    std::vector<std::size_t> participatingSeats() const;
    std::optional<std::size_t> nextParticipatingSeat(std::size_t from) const;
    std::optional<std::size_t> nextCanActSeat(std::size_t from) const;
    std::optional<std::size_t> firstPostflopActor() const;
    std::optional<std::size_t> seatOf(PlayerId id) const;
    void dealHoleCards(const std::vector<std::size_t>& seats);
    void postBlind(Player& player, Chips blind);
    void setInitialActor(std::size_t big_blind_seat, std::size_t participant_count);
    bool canAct(const Player& player) const noexcept;
    std::size_t contenders() const noexcept;
    void removeFromActionSets(PlayerId id);
    void resetAfterFullRaise(PlayerId raiser);
    void addUnderCalledPlayers(PlayerId actor);
    bool bettingRoundComplete() const;
    void selectNextActor(std::size_t previous_seat);
    void advanceUntilActionOrSettlement(ActionResult& result);
    void advanceStreet();
    void revealFlop();
    void revealOne();
    void settleFoldWin(ActionResult& result);
    void settleShowdown(ActionResult& result);
    std::vector<PotAward> buildAndAwardPots();
    std::vector<PlayerId> orderedClockwise(const std::vector<PlayerId>& ids) const;
    Chips potSize() const noexcept;
    void markSettled();
    ActionResult fail(TableError error, std::string message) const;
    ActionResult succeed();

    TableConfig config_;
    std::vector<std::optional<Player>> seats_;
    Street street_{Street::waiting};
    std::optional<std::size_t> dealer_seat_;
    std::optional<std::size_t> acting_seat_;
    Chips current_bet_{0};
    Chips minimum_raise_{0};
    std::unordered_set<PlayerId> pending_action_;
    std::unordered_set<PlayerId> raise_allowed_;
    std::vector<Card> board_;
    std::unique_ptr<Deck> deck_;
    std::uint64_t hand_id_{0};
    std::uint64_t server_sequence_{0};
    std::vector<PotAward> last_awards_;
};

std::string toString(TableError error);

}  // namespace poker::domain

#endif
