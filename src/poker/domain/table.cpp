#include "poker/domain/table.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace poker::domain {
namespace {

bool handIsRunning(Street street) noexcept {
    return street >= Street::preflop && street <= Street::showdown;
}

}  // namespace

Table::Table(TableConfig config)
    : config_(config), seats_(config.max_players), minimum_raise_(config.big_blind) {}

TableError Table::validateConfig() const noexcept {
    if (config_.min_players < 2 || config_.max_players < config_.min_players
        || config_.max_players > 6 || config_.small_blind <= 0
        || config_.big_blind < config_.small_blind * 2 || config_.min_buy_in <= 0
        || config_.max_buy_in < config_.min_buy_in) {
        return TableError::invalid_configuration;
    }
    return TableError::ok;
}

TableError Table::seatPlayer(PlayerId id, std::size_t seat, Chips buy_in) {
    if (validateConfig() != TableError::ok) {
        return TableError::invalid_configuration;
    }
    if (handIsRunning(street_)) {
        return TableError::hand_in_progress;
    }
    if (seat >= seats_.size()) {
        return TableError::seat_out_of_range;
    }
    if (findPlayer(id) != nullptr) {
        return TableError::player_already_seated;
    }
    if (seats_[seat].has_value()) {
        return TableError::seat_occupied;
    }
    if (buy_in < config_.min_buy_in || buy_in > config_.max_buy_in) {
        return TableError::invalid_buy_in;
    }

    Player player;
    player.id = id;
    player.seat = seat;
    player.stack = buy_in;
    seats_[seat] = player;
    ++server_sequence_;
    return TableError::ok;
}

TableError Table::removePlayer(PlayerId id) {
    const auto seat = seatOf(id);
    if (!seat.has_value()) {
        return TableError::player_not_found;
    }
    if (handIsRunning(street_) && seats_[*seat]->status != PlayerStatus::waiting) {
        return TableError::hand_in_progress;
    }
    seats_[*seat].reset();
    if (dealer_seat_ == seat) {
        dealer_seat_.reset();
    }
    ++server_sequence_;
    return TableError::ok;
}

TableError Table::setReady(PlayerId id, bool ready) {
    if (handIsRunning(street_)) {
        return TableError::hand_in_progress;
    }
    auto* player = findPlayer(id);
    if (player == nullptr) {
        return TableError::player_not_found;
    }
    if (ready && player->stack <= 0) {
        return TableError::invalid_buy_in;
    }
    player->ready = ready;
    ++server_sequence_;
    return TableError::ok;
}

TableError Table::setConnected(PlayerId id, bool connected) {
    auto* player = findPlayer(id);
    if (player == nullptr) {
        return TableError::player_not_found;
    }
    player->connected = connected;
    ++server_sequence_;
    return TableError::ok;
}

TableError Table::startHand(RandomSource& random) {
    return startHand(std::make_unique<Deck>(random));
}

TableError Table::startHandWithDeck(std::vector<Card> cards) {
    try {
        return startHand(std::make_unique<Deck>(std::move(cards)));
    } catch (const std::invalid_argument&) {
        return TableError::invalid_configuration;
    }
}

TableError Table::abortHand() {
    if (street_ == Street::waiting) {
        return TableError::action_not_allowed;
    }
    if (street_ == Street::settled) {
        for (const auto& award : last_awards_) {
            for (std::size_t index = 0; index < award.winners.size(); ++index) {
                auto* winner = findPlayer(award.winners[index]);
                if (winner != nullptr) {
                    winner->stack -= award.equal_share
                                     + (static_cast<Chips>(index) < award.odd_chips ? 1 : 0);
                }
            }
        }
    }
    for (auto& seat : seats_) {
        if (!seat.has_value()) {
            continue;
        }
        seat->stack += seat->hand_commitment;
        seat->street_commitment = 0;
        seat->hand_commitment = 0;
        seat->status = PlayerStatus::waiting;
        seat->ready = false;
        seat->has_hole = false;
    }
    street_ = Street::waiting;
    acting_seat_.reset();
    current_bet_ = 0;
    minimum_raise_ = config_.big_blind;
    pending_action_.clear();
    raise_allowed_.clear();
    board_.clear();
    deck_.reset();
    last_awards_.clear();
    ++server_sequence_;
    return TableError::ok;
}

TableError Table::startHand(std::unique_ptr<Deck> deck) {
    if (validateConfig() != TableError::ok) {
        return TableError::invalid_configuration;
    }
    if (handIsRunning(street_)) {
        return TableError::hand_in_progress;
    }

    std::size_t ready_players = 0;
    for (const auto& seat : seats_) {
        if (seat.has_value() && seat->ready && seat->stack > 0) {
            ++ready_players;
        }
    }
    if (ready_players < config_.min_players) {
        return TableError::not_enough_players;
    }

    for (auto& seat : seats_) {
        if (!seat.has_value()) {
            continue;
        }
        auto& player = *seat;
        player.street_commitment = 0;
        player.hand_commitment = 0;
        player.has_hole = false;
        player.status = player.ready && player.stack > 0 ? PlayerStatus::active : PlayerStatus::waiting;
    }

    deck_ = std::move(deck);
    board_.clear();
    last_awards_.clear();
    current_bet_ = 0;
    minimum_raise_ = config_.big_blind;
    pending_action_.clear();
    raise_allowed_.clear();
    acting_seat_.reset();
    street_ = Street::preflop;
    ++hand_id_;
    ++server_sequence_;

    const auto participants = participatingSeats();
    if (participants.size() < config_.min_players) {
        street_ = Street::waiting;
        return TableError::not_enough_players;
    }

    if (!dealer_seat_.has_value()) {
        dealer_seat_ = participants.front();
    } else {
        const auto next = nextParticipatingSeat(*dealer_seat_);
        dealer_seat_ = next.has_value() ? *next : participants.front();
    }

    dealHoleCards(participants);

    std::size_t small_blind_seat = *dealer_seat_;
    if (participants.size() > 2) {
        small_blind_seat = *nextParticipatingSeat(*dealer_seat_);
    }
    const auto big_blind_seat = *nextParticipatingSeat(small_blind_seat);
    postBlind(*seats_[small_blind_seat], config_.small_blind);
    postBlind(*seats_[big_blind_seat], config_.big_blind);
    current_bet_ = std::max(seats_[small_blind_seat]->street_commitment,
                            seats_[big_blind_seat]->street_commitment);

    for (const auto seat : participants) {
        auto& player = *seats_[seat];
        if (canAct(player)) {
            pending_action_.insert(player.id);
            raise_allowed_.insert(player.id);
        }
    }
    setInitialActor(big_blind_seat, participants.size());

    ActionResult automatic;
    automatic.error = TableError::ok;
    const auto actionable = std::count_if(seats_.begin(), seats_.end(), [this](const auto& seat) {
        return seat.has_value() && canAct(*seat);
    });
    if (actionable <= 1) {
        pending_action_.clear();
        acting_seat_.reset();
        advanceUntilActionOrSettlement(automatic);
    }
    return TableError::ok;
}

ActionResult Table::act(const ActionCommand& command) {
    if (!handIsRunning(street_) || street_ == Street::showdown) {
        return fail(TableError::action_not_allowed, "no betting round is active");
    }
    if (!acting_seat_.has_value() || !seats_[*acting_seat_].has_value()
        || seats_[*acting_seat_]->id != command.player_id) {
        return fail(TableError::not_players_turn, "the command was not sent by the acting player");
    }

    auto& player = *seats_[*acting_seat_];
    if (!canAct(player) || pending_action_.count(player.id) == 0) {
        return fail(TableError::player_cannot_act, "the player cannot act in this betting round");
    }

    const auto action_street = street_;
    const auto previous_seat = player.seat;
    const auto available = player.street_commitment + player.stack;
    const auto payTo = [&player](Chips target) {
        const auto payment = target - player.street_commitment;
        player.stack -= payment;
        player.street_commitment += payment;
        player.hand_commitment += payment;
        if (player.stack == 0) {
            player.status = PlayerStatus::all_in;
        }
    };

    switch (command.type) {
    case ActionType::fold:
        player.status = PlayerStatus::folded;
        removeFromActionSets(player.id);
        break;

    case ActionType::check:
        if (player.street_commitment != current_bet_) {
            return fail(TableError::action_not_allowed, "check is only legal when no chips are owed");
        }
        removeFromActionSets(player.id);
        break;

    case ActionType::call: {
        if (player.street_commitment >= current_bet_) {
            return fail(TableError::action_not_allowed, "there is no bet to call");
        }
        payTo(std::min(current_bet_, available));
        removeFromActionSets(player.id);
        break;
    }

    case ActionType::bet: {
        if (current_bet_ != 0) {
            return fail(TableError::action_not_allowed, "an existing bet must be raised, not bet again");
        }
        if (command.amount <= player.street_commitment || command.amount > available) {
            return fail(TableError::invalid_amount, "bet target is outside the player's available stack");
        }
        const bool all_in = command.amount == available;
        if (command.amount < minimum_raise_ && !all_in) {
            return fail(TableError::minimum_raise_not_met, "bet is below the table minimum");
        }
        const bool full_raise = command.amount >= minimum_raise_;
        payTo(command.amount);
        current_bet_ = command.amount;
        if (full_raise) {
            minimum_raise_ = command.amount;
            resetAfterFullRaise(player.id);
        } else {
            addUnderCalledPlayers(player.id);
            removeFromActionSets(player.id);
        }
        break;
    }

    case ActionType::raise: {
        if (current_bet_ == 0) {
            return fail(TableError::action_not_allowed, "raise requires an existing bet");
        }
        if (raise_allowed_.count(player.id) == 0) {
            return fail(TableError::action_not_allowed, "a short all-in did not reopen raising");
        }
        if (command.amount <= current_bet_ || command.amount > available) {
            return fail(TableError::invalid_amount, "raise target must exceed the current bet and fit the stack");
        }
        const auto raise_size = command.amount - current_bet_;
        const bool all_in = command.amount == available;
        if (raise_size < minimum_raise_ && !all_in) {
            return fail(TableError::minimum_raise_not_met, "raise increment is below the minimum raise");
        }
        payTo(command.amount);
        current_bet_ = command.amount;
        if (raise_size >= minimum_raise_) {
            minimum_raise_ = raise_size;
            resetAfterFullRaise(player.id);
        } else {
            addUnderCalledPlayers(player.id);
            removeFromActionSets(player.id);
        }
        break;
    }

    case ActionType::all_in: {
        if (player.stack <= 0) {
            return fail(TableError::player_cannot_act, "the player has no chips left");
        }
        const auto target = available;
        if (target <= current_bet_) {
            payTo(target);
            removeFromActionSets(player.id);
            break;
        }
        if (raise_allowed_.count(player.id) == 0) {
            return fail(TableError::action_not_allowed, "raising was not reopened for this player");
        }
        const auto raise_size = target - current_bet_;
        payTo(target);
        current_bet_ = target;
        if (raise_size >= minimum_raise_) {
            minimum_raise_ = raise_size;
            resetAfterFullRaise(player.id);
        } else {
            addUnderCalledPlayers(player.id);
            removeFromActionSets(player.id);
        }
        break;
    }
    }

    auto result = succeed();
    result.action_street = action_street;
    if (contenders() <= 1) {
        settleFoldWin(result);
        result.server_sequence = server_sequence_;
        return result;
    }

    if (bettingRoundComplete()) {
        acting_seat_.reset();
        advanceUntilActionOrSettlement(result);
    } else {
        selectNextActor(previous_seat);
    }
    result.server_sequence = server_sequence_;
    if (street_ == Street::settled) {
        result.awards = last_awards_;
    }
    return result;
}

TableSnapshot Table::snapshot(std::optional<PlayerId> viewer) const {
    TableSnapshot result;
    result.street = street_;
    result.hand_id = hand_id_;
    result.server_sequence = server_sequence_;
    result.current_bet = current_bet_;
    result.minimum_raise = minimum_raise_;
    result.pot = street_ == Street::settled ? 0 : potSize();
    result.board = board_;

    if (dealer_seat_.has_value() && seats_[*dealer_seat_].has_value()) {
        result.dealer = seats_[*dealer_seat_]->id;
    }
    if (acting_seat_.has_value() && seats_[*acting_seat_].has_value()) {
        result.acting_player = seats_[*acting_seat_]->id;
    }

    for (const auto& seat : seats_) {
        if (!seat.has_value()) {
            continue;
        }
        const auto& player = *seat;
        PlayerView view;
        view.id = player.id;
        view.seat = player.seat;
        view.stack = player.stack;
        view.street_commitment = player.street_commitment;
        view.hand_commitment = player.hand_commitment;
        view.status = player.status;
        view.ready = player.ready;
        view.connected = player.connected;
        const bool own_cards = viewer.has_value() && *viewer == player.id;
        const bool showdown_cards = street_ == Street::settled && player.status != PlayerStatus::folded
                                    && player.status != PlayerStatus::waiting;
        if (player.has_hole && (own_cards || showdown_cards)) {
            view.hole_cards.assign(player.hole.begin(), player.hole.end());
        }
        result.players.push_back(std::move(view));
    }
    return result;
}

TableSnapshot Table::auditSnapshot() const {
    auto result = snapshot();
    for (auto& view : result.players) {
        const auto* source = findPlayer(view.id);
        if (source != nullptr && source->has_hole) {
            view.hole_cards.assign(source->hole.begin(), source->hole.end());
        }
    }
    return result;
}

const std::vector<PotAward>& Table::lastAwards() const noexcept {
    return last_awards_;
}

Chips Table::totalChips() const noexcept {
    Chips total = 0;
    for (const auto& seat : seats_) {
        if (!seat.has_value()) {
            continue;
        }
        total += seat->stack;
        if (street_ != Street::settled) {
            total += seat->hand_commitment;
        }
    }
    return total;
}

Table::Player* Table::findPlayer(PlayerId id) noexcept {
    for (auto& seat : seats_) {
        if (seat.has_value() && seat->id == id) {
            return &*seat;
        }
    }
    return nullptr;
}

const Table::Player* Table::findPlayer(PlayerId id) const noexcept {
    for (const auto& seat : seats_) {
        if (seat.has_value() && seat->id == id) {
            return &*seat;
        }
    }
    return nullptr;
}

std::vector<std::size_t> Table::participatingSeats() const {
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < seats_.size(); ++index) {
        if (seats_[index].has_value() && seats_[index]->status == PlayerStatus::active) {
            result.push_back(index);
        }
    }
    return result;
}

std::optional<std::size_t> Table::nextParticipatingSeat(std::size_t from) const {
    for (std::size_t offset = 1; offset <= seats_.size(); ++offset) {
        const auto index = (from + offset) % seats_.size();
        if (!seats_[index].has_value()) {
            continue;
        }
        const auto status = seats_[index]->status;
        if (status == PlayerStatus::active || status == PlayerStatus::all_in
            || status == PlayerStatus::folded) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Table::nextCanActSeat(std::size_t from) const {
    for (std::size_t offset = 1; offset <= seats_.size(); ++offset) {
        const auto index = (from + offset) % seats_.size();
        if (seats_[index].has_value() && canAct(*seats_[index])
            && pending_action_.count(seats_[index]->id) > 0) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Table::firstPostflopActor() const {
    if (!dealer_seat_.has_value()) {
        return std::nullopt;
    }
    return nextCanActSeat(*dealer_seat_);
}

std::optional<std::size_t> Table::seatOf(PlayerId id) const {
    for (std::size_t index = 0; index < seats_.size(); ++index) {
        if (seats_[index].has_value() && seats_[index]->id == id) {
            return index;
        }
    }
    return std::nullopt;
}

void Table::dealHoleCards(const std::vector<std::size_t>& seats) {
    if (!dealer_seat_.has_value()) {
        throw std::logic_error("dealer must be selected before dealing");
    }
    auto seat = nextParticipatingSeat(*dealer_seat_);
    for (int round = 0; round < 2; ++round) {
        for (std::size_t dealt = 0; dealt < seats.size(); ++dealt) {
            auto& player = *seats_[*seat];
            player.hole[static_cast<std::size_t>(round)] = deck_->draw();
            player.has_hole = true;
            seat = nextParticipatingSeat(*seat);
        }
    }
}

void Table::postBlind(Player& player, Chips blind) {
    const auto payment = std::min(blind, player.stack);
    player.stack -= payment;
    player.street_commitment += payment;
    player.hand_commitment += payment;
    if (player.stack == 0) {
        player.status = PlayerStatus::all_in;
    }
}

void Table::setInitialActor(std::size_t big_blind_seat, std::size_t participant_count) {
    if (participant_count == 2 && dealer_seat_.has_value()) {
        if (seats_[*dealer_seat_].has_value() && canAct(*seats_[*dealer_seat_])
            && pending_action_.count(seats_[*dealer_seat_]->id) > 0) {
            acting_seat_ = *dealer_seat_;
            return;
        }
        acting_seat_ = nextCanActSeat(*dealer_seat_);
        return;
    }
    acting_seat_ = nextCanActSeat(big_blind_seat);
}

bool Table::canAct(const Player& player) const noexcept {
    return player.status == PlayerStatus::active && player.stack > 0;
}

std::size_t Table::contenders() const noexcept {
    return static_cast<std::size_t>(std::count_if(seats_.begin(), seats_.end(), [](const auto& seat) {
        if (!seat.has_value()) {
            return false;
        }
        return seat->status == PlayerStatus::active || seat->status == PlayerStatus::all_in
               || seat->status == PlayerStatus::disconnected;
    }));
}

void Table::removeFromActionSets(PlayerId id) {
    pending_action_.erase(id);
    raise_allowed_.erase(id);
}

void Table::resetAfterFullRaise(PlayerId raiser) {
    pending_action_.clear();
    raise_allowed_.clear();
    for (const auto& seat : seats_) {
        if (!seat.has_value() || seat->id == raiser || !canAct(*seat)) {
            continue;
        }
        pending_action_.insert(seat->id);
        raise_allowed_.insert(seat->id);
    }
}

void Table::addUnderCalledPlayers(PlayerId actor) {
    for (const auto& seat : seats_) {
        if (!seat.has_value() || seat->id == actor || !canAct(*seat)) {
            continue;
        }
        if (seat->street_commitment < current_bet_) {
            pending_action_.insert(seat->id);
        }
    }
}

bool Table::bettingRoundComplete() const {
    if (contenders() <= 1) {
        return true;
    }
    for (const auto& seat : seats_) {
        if (!seat.has_value() || !canAct(*seat)) {
            continue;
        }
        if (pending_action_.count(seat->id) > 0 || seat->street_commitment != current_bet_) {
            return false;
        }
    }
    return true;
}

void Table::selectNextActor(std::size_t previous_seat) {
    acting_seat_ = nextCanActSeat(previous_seat);
}

void Table::advanceUntilActionOrSettlement(ActionResult& result) {
    while (true) {
        if (contenders() <= 1) {
            settleFoldWin(result);
            return;
        }

        if (!bettingRoundComplete()) {
            if (!acting_seat_.has_value()) {
                if (street_ == Street::preflop && dealer_seat_.has_value()) {
                    acting_seat_ = nextCanActSeat(*dealer_seat_);
                } else {
                    acting_seat_ = firstPostflopActor();
                }
            }
            return;
        }

        if (street_ == Street::river) {
            settleShowdown(result);
            return;
        }

        advanceStreet();
        const auto actionable = std::count_if(seats_.begin(), seats_.end(), [this](const auto& seat) {
            return seat.has_value() && canAct(*seat);
        });
        if (actionable <= 1) {
            pending_action_.clear();
            acting_seat_.reset();
            continue;
        }
        acting_seat_ = firstPostflopActor();
        return;
    }
}

void Table::advanceStreet() {
    for (auto& seat : seats_) {
        if (seat.has_value()) {
            seat->street_commitment = 0;
        }
    }
    current_bet_ = 0;
    minimum_raise_ = config_.big_blind;
    pending_action_.clear();
    raise_allowed_.clear();

    switch (street_) {
    case Street::preflop:
        street_ = Street::flop;
        revealFlop();
        break;
    case Street::flop:
        street_ = Street::turn;
        revealOne();
        break;
    case Street::turn:
        street_ = Street::river;
        revealOne();
        break;
    default:
        throw std::logic_error("cannot advance the current street");
    }

    for (const auto& seat : seats_) {
        if (seat.has_value() && canAct(*seat)) {
            pending_action_.insert(seat->id);
            raise_allowed_.insert(seat->id);
        }
    }
}

void Table::revealFlop() {
    deck_->burn();
    board_.push_back(deck_->draw());
    board_.push_back(deck_->draw());
    board_.push_back(deck_->draw());
}

void Table::revealOne() {
    deck_->burn();
    board_.push_back(deck_->draw());
}

void Table::settleFoldWin(ActionResult& result) {
    Player* winner = nullptr;
    for (auto& seat : seats_) {
        if (!seat.has_value()) {
            continue;
        }
        if (seat->status == PlayerStatus::active || seat->status == PlayerStatus::all_in
            || seat->status == PlayerStatus::disconnected) {
            winner = &*seat;
            break;
        }
    }
    if (winner == nullptr) {
        throw std::logic_error("fold settlement has no winner");
    }

    const auto pot = potSize();
    winner->stack += pot;
    last_awards_ = {{pot, {winner->id}, pot, 0}};
    result.awards = last_awards_;
    markSettled();
}

void Table::settleShowdown(ActionResult& result) {
    street_ = Street::showdown;
    while (board_.size() < 5) {
        if (board_.empty()) {
            revealFlop();
        } else {
            revealOne();
        }
    }
    last_awards_ = buildAndAwardPots();
    result.awards = last_awards_;
    markSettled();
}

std::vector<PotAward> Table::buildAndAwardPots() {
    std::vector<Chips> levels;
    for (const auto& seat : seats_) {
        if (seat.has_value() && seat->hand_commitment > 0) {
            levels.push_back(seat->hand_commitment);
        }
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

    std::vector<PotAward> awards;
    Chips previous = 0;
    for (const auto level : levels) {
        std::size_t contributors = 0;
        std::vector<Player*> eligible;
        for (auto& seat : seats_) {
            if (!seat.has_value() || seat->hand_commitment < level) {
                continue;
            }
            ++contributors;
            if (seat->status != PlayerStatus::folded && seat->status != PlayerStatus::waiting) {
                eligible.push_back(&*seat);
            }
        }
        const auto pot = (level - previous) * static_cast<Chips>(contributors);
        previous = level;
        if (pot <= 0 || eligible.empty()) {
            continue;
        }

        HandValue best{};
        bool initialized = false;
        std::vector<PlayerId> winners;
        for (const auto* player : eligible) {
            std::vector<Card> cards(board_.begin(), board_.end());
            cards.push_back(player->hole[0]);
            cards.push_back(player->hole[1]);
            const auto value = evaluateBest(cards);
            if (!initialized || best < value) {
                best = value;
                winners = {player->id};
                initialized = true;
            } else if (value == best) {
                winners.push_back(player->id);
            }
        }

        winners = orderedClockwise(winners);
        const auto share = pot / static_cast<Chips>(winners.size());
        const auto odd = pot % static_cast<Chips>(winners.size());
        for (std::size_t index = 0; index < winners.size(); ++index) {
            auto* player = findPlayer(winners[index]);
            player->stack += share + (static_cast<Chips>(index) < odd ? 1 : 0);
        }
        awards.push_back({pot, winners, share, odd});
    }
    return awards;
}

std::vector<PlayerId> Table::orderedClockwise(const std::vector<PlayerId>& ids) const {
    std::vector<PlayerId> result = ids;
    const auto origin = dealer_seat_.value_or(0);
    std::sort(result.begin(), result.end(), [this, origin](PlayerId lhs, PlayerId rhs) {
        const auto lhs_seat = seatOf(lhs).value();
        const auto rhs_seat = seatOf(rhs).value();
        auto lhs_distance = (lhs_seat + seats_.size() - origin) % seats_.size();
        auto rhs_distance = (rhs_seat + seats_.size() - origin) % seats_.size();
        // Odd chips are awarded clockwise starting to the dealer's left. The
        // dealer is therefore considered last when the dealer is also tied.
        lhs_distance = lhs_distance == 0 ? seats_.size() : lhs_distance;
        rhs_distance = rhs_distance == 0 ? seats_.size() : rhs_distance;
        return lhs_distance < rhs_distance;
    });
    return result;
}

Chips Table::potSize() const noexcept {
    Chips pot = 0;
    for (const auto& seat : seats_) {
        if (seat.has_value()) {
            pot += seat->hand_commitment;
        }
    }
    return pot;
}

void Table::markSettled() {
    for (auto& seat : seats_) {
        if (seat.has_value()) {
            seat->ready = false;
        }
    }
    street_ = Street::settled;
    acting_seat_.reset();
    pending_action_.clear();
    raise_allowed_.clear();
    current_bet_ = 0;
    ++server_sequence_;
}

ActionResult Table::fail(TableError error, std::string message) const {
    return {error, std::move(message), server_sequence_, {}};
}

ActionResult Table::succeed() {
    ++server_sequence_;
    return {TableError::ok, {}, server_sequence_, {}};
}

std::string toString(TableError error) {
    switch (error) {
    case TableError::ok: return "ok";
    case TableError::invalid_configuration: return "invalid configuration";
    case TableError::seat_out_of_range: return "seat out of range";
    case TableError::seat_occupied: return "seat occupied";
    case TableError::player_already_seated: return "player already seated";
    case TableError::player_not_found: return "player not found";
    case TableError::invalid_buy_in: return "invalid buy-in";
    case TableError::hand_in_progress: return "hand in progress";
    case TableError::not_enough_players: return "not enough players";
    case TableError::player_not_ready: return "player not ready";
    case TableError::not_players_turn: return "not player's turn";
    case TableError::player_cannot_act: return "player cannot act";
    case TableError::action_not_allowed: return "action not allowed";
    case TableError::invalid_amount: return "invalid amount";
    case TableError::minimum_raise_not_met: return "minimum raise not met";
    case TableError::service_unavailable: return "service unavailable";
    }
    return "unknown table error";
}

}  // namespace poker::domain
