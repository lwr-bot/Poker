#include "poker/domain/card.hpp"
#include "poker/domain/hand_evaluator.hpp"
#include "poker/domain/table.hpp"
#include "poker/application/idempotency_cache.hpp"
#include "poker/application/room_executor.hpp"
#include "poker/application/room_manager.hpp"
#include "poker/cluster/in_memory_node_registry.hpp"
#include "poker/cluster/lobby_router.hpp"
#include "poker/cluster/node_failure_reaper.hpp"
#include "poker/net/length_field_codec.hpp"
#include "poker/net/token_bucket.hpp"
#include "poker/observability/metrics.hpp"
#include "poker/security/auth_service.hpp"
#include "poker/storage/in_memory_account_store.hpp"
#include "poker/storage/in_memory_game_store.hpp"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using poker::domain::ActionCommand;
using poker::domain::ActionType;
using poker::domain::Card;
using poker::domain::Chips;
using poker::domain::HandCategory;
using poker::domain::PlayerId;
using poker::domain::Rank;
using poker::domain::Street;
using poker::domain::Suit;
using poker::domain::Table;
using poker::domain::TableConfig;
using poker::domain::TableError;

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string location(const char* file, int line) {
    return std::string(file) + ":" + std::to_string(line);
}

#define REQUIRE(condition)                                                                            \
    do {                                                                                              \
        if (!(condition)) {                                                                           \
            throw TestFailure(location(__FILE__, __LINE__) + " requirement failed: " #condition);   \
        }                                                                                             \
    } while (false)

#define REQUIRE_EQ(actual, expected)                                                                  \
    do {                                                                                              \
        const auto actual_value = (actual);                                                           \
        const auto expected_value = (expected);                                                       \
        if (!(actual_value == expected_value)) {                                                       \
            throw TestFailure(location(__FILE__, __LINE__) + " equality requirement failed");        \
        }                                                                                             \
    } while (false)

Card c(Rank rank, Suit suit) {
    return {rank, suit};
}

std::vector<Card> deckWithPrefix(std::vector<Card> prefix) {
    std::array<bool, 52> used{};
    for (const auto card : prefix) {
        const auto index = poker::domain::cardIndex(card);
        REQUIRE(index < used.size());
        REQUIRE(!used[index]);
        used[index] = true;
    }
    for (const auto card : poker::domain::orderedDeck()) {
        if (!used[poker::domain::cardIndex(card)]) {
            prefix.push_back(card);
        }
    }
    REQUIRE_EQ(prefix.size(), std::size_t{52});
    return prefix;
}

TableConfig config(Chips minimum = 100, Chips maximum = 10'000) {
    TableConfig result;
    result.small_blind = 5;
    result.big_blind = 10;
    result.min_buy_in = minimum;
    result.max_buy_in = maximum;
    return result;
}

const poker::domain::PlayerView& player(const poker::domain::TableSnapshot& snapshot, PlayerId id) {
    for (const auto& candidate : snapshot.players) {
        if (candidate.id == id) {
            return candidate;
        }
    }
    throw TestFailure("player not found in snapshot");
}

void seatAndReady(Table& table, PlayerId id, std::size_t seat, Chips chips) {
    REQUIRE_EQ(table.seatPlayer(id, seat, chips), TableError::ok);
    REQUIRE_EQ(table.setReady(id, true), TableError::ok);
}

void checkByActingPlayer(Table& table) {
    const auto snapshot = table.snapshot();
    REQUIRE(snapshot.acting_player.has_value());
    const auto result = table.act({*snapshot.acting_player, ActionType::check, 0});
    REQUIRE(result);
}

void handCategoryExamples() {
    REQUIRE_EQ(poker::domain::evaluateFive({c(Rank::ace, Suit::spades),
                                             c(Rank::king, Suit::spades),
                                             c(Rank::queen, Suit::spades),
                                             c(Rank::jack, Suit::spades),
                                             c(Rank::ten, Suit::spades)}).category,
               HandCategory::straight_flush);
    REQUIRE_EQ(poker::domain::evaluateFive({c(Rank::ace, Suit::clubs),
                                             c(Rank::ace, Suit::diamonds),
                                             c(Rank::ace, Suit::hearts),
                                             c(Rank::ace, Suit::spades),
                                             c(Rank::king, Suit::spades)}).category,
               HandCategory::four_of_a_kind);
    REQUIRE_EQ(poker::domain::evaluateFive({c(Rank::queen, Suit::clubs),
                                             c(Rank::queen, Suit::diamonds),
                                             c(Rank::queen, Suit::hearts),
                                             c(Rank::two, Suit::spades),
                                             c(Rank::two, Suit::clubs)}).category,
               HandCategory::full_house);
    REQUIRE_EQ(poker::domain::evaluateFive({c(Rank::ace, Suit::clubs),
                                             c(Rank::two, Suit::diamonds),
                                             c(Rank::three, Suit::hearts),
                                             c(Rank::four, Suit::spades),
                                             c(Rank::five, Suit::clubs)}).tiebreak[0],
               std::uint8_t{5});

    const auto seven = poker::domain::evaluateBest({c(Rank::ace, Suit::spades),
                                                     c(Rank::king, Suit::spades),
                                                     c(Rank::queen, Suit::spades),
                                                     c(Rank::jack, Suit::spades),
                                                     c(Rank::ten, Suit::spades),
                                                     c(Rank::two, Suit::clubs),
                                                     c(Rank::two, Suit::diamonds)});
    REQUIRE_EQ(seven.category, HandCategory::straight_flush);
}

void exhaustiveFiveCardDistribution() {
    const auto deck = poker::domain::orderedDeck();
    std::array<std::uint64_t, 9> counts{};
    std::uint64_t total = 0;
    for (std::size_t a = 0; a < 48; ++a) {
        for (std::size_t b = a + 1; b < 49; ++b) {
            for (std::size_t d = b + 1; d < 50; ++d) {
                for (std::size_t e = d + 1; e < 51; ++e) {
                    for (std::size_t f = e + 1; f < 52; ++f) {
                        const auto value = poker::domain::evaluateFive(
                            {deck[a], deck[b], deck[d], deck[e], deck[f]});
                        ++counts[static_cast<std::size_t>(value.category)];
                        ++total;
                    }
                }
            }
        }
    }

    REQUIRE_EQ(total, std::uint64_t{2'598'960});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::high_card)], std::uint64_t{1'302'540});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::one_pair)], std::uint64_t{1'098'240});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::two_pair)], std::uint64_t{123'552});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::three_of_a_kind)], std::uint64_t{54'912});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::straight)], std::uint64_t{10'200});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::flush)], std::uint64_t{5'108});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::full_house)], std::uint64_t{3'744});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::four_of_a_kind)], std::uint64_t{624});
    REQUIRE_EQ(counts[static_cast<std::size_t>(HandCategory::straight_flush)], std::uint64_t{40});
}

void headsUpOrderAndFoldSettlement() {
    Table table(config());
    seatAndReady(table, 1, 0, 1'000);
    seatAndReady(table, 2, 1, 1'000);
    REQUIRE_EQ(table.startHandWithDeck(deckWithPrefix({})), TableError::ok);

    auto snapshot = table.snapshot(1);
    REQUIRE_EQ(snapshot.street, Street::preflop);
    REQUIRE_EQ(snapshot.dealer, std::optional<PlayerId>{1});
    REQUIRE_EQ(snapshot.acting_player, std::optional<PlayerId>{1});
    REQUIRE_EQ(player(snapshot, 1).street_commitment, Chips{5});
    REQUIRE_EQ(player(snapshot, 2).street_commitment, Chips{10});
    REQUIRE_EQ(player(snapshot, 1).hole_cards.size(), std::size_t{2});
    REQUIRE_EQ(player(table.snapshot(2), 1).hole_cards.size(), std::size_t{0});
    REQUIRE_EQ(player(table.auditSnapshot(), 1).hole_cards.size(), std::size_t{2});
    REQUIRE_EQ(player(table.auditSnapshot(), 2).hole_cards.size(), std::size_t{2});

    const auto folded = table.act({1, ActionType::fold, 0});
    REQUIRE(folded);
    REQUIRE_EQ(table.snapshot().street, Street::settled);
    REQUIRE_EQ(player(table.snapshot(), 1).stack, Chips{995});
    REQUIRE_EQ(player(table.snapshot(), 2).stack, Chips{1'005});
    REQUIRE_EQ(table.totalChips(), Chips{2'000});
    REQUIRE_EQ(table.abortHand(), TableError::ok);
    REQUIRE_EQ(table.snapshot().street, Street::waiting);
    REQUIRE_EQ(player(table.snapshot(), 1).stack, Chips{1'000});
    REQUIRE_EQ(player(table.snapshot(), 2).stack, Chips{1'000});
    REQUIRE_EQ(table.totalChips(), Chips{2'000});
}

void headsUpStreetOrderAndShowdown() {
    Table table(config());
    seatAndReady(table, 1, 0, 1'000);
    seatAndReady(table, 2, 1, 1'000);
    REQUIRE_EQ(table.startHandWithDeck(deckWithPrefix({})), TableError::ok);

    REQUIRE(table.act({1, ActionType::call, 0}));
    REQUIRE(table.act({2, ActionType::check, 0}));
    REQUIRE_EQ(table.snapshot().street, Street::flop);
    REQUIRE_EQ(table.snapshot().acting_player, std::optional<PlayerId>{2});

    while (table.snapshot().street != Street::settled) {
        checkByActingPlayer(table);
    }
    REQUIRE_EQ(table.snapshot().board.size(), std::size_t{5});
    REQUIRE_EQ(table.totalChips(), Chips{2'000});

    REQUIRE(!player(table.snapshot(), 1).ready);
    REQUIRE(!player(table.snapshot(), 2).ready);
    REQUIRE_EQ(table.setReady(1, true), TableError::ok);
    REQUIRE_EQ(table.startHandWithDeck(deckWithPrefix({})), TableError::not_enough_players);
    REQUIRE_EQ(table.setReady(2, true), TableError::ok);
    REQUIRE_EQ(table.startHandWithDeck(deckWithPrefix({})), TableError::ok);
    REQUIRE_EQ(table.snapshot().hand_id, std::uint64_t{2});
    REQUIRE_EQ(table.totalChips(), Chips{2'000});
}

void multiLevelSidePots() {
    Table table(config());
    seatAndReady(table, 1, 0, 100);
    seatAndReady(table, 2, 1, 200);
    seatAndReady(table, 3, 2, 300);

    const auto deck = deckWithPrefix({
        c(Rank::king, Suit::clubs), c(Rank::three, Suit::clubs), c(Rank::ace, Suit::clubs),
        c(Rank::king, Suit::diamonds), c(Rank::four, Suit::diamonds), c(Rank::ace, Suit::diamonds),
        c(Rank::five, Suit::spades), c(Rank::two, Suit::hearts), c(Rank::seven, Suit::hearts),
        c(Rank::nine, Suit::spades), c(Rank::six, Suit::spades), c(Rank::jack, Suit::hearts),
        c(Rank::eight, Suit::spades), c(Rank::queen, Suit::spades),
    });
    REQUIRE_EQ(table.startHandWithDeck(deck), TableError::ok);
    REQUIRE(table.act({1, ActionType::all_in, 0}));
    REQUIRE(table.act({2, ActionType::all_in, 0}));
    REQUIRE(table.act({3, ActionType::call, 0}));

    REQUIRE_EQ(table.snapshot().street, Street::settled);
    REQUIRE_EQ(table.lastAwards().size(), std::size_t{2});
    REQUIRE_EQ(table.lastAwards()[0].pot_size, Chips{300});
    REQUIRE_EQ(table.lastAwards()[0].winners, std::vector<PlayerId>{1});
    REQUIRE_EQ(table.lastAwards()[1].pot_size, Chips{200});
    REQUIRE_EQ(table.lastAwards()[1].winners, std::vector<PlayerId>{2});
    REQUIRE_EQ(player(table.snapshot(), 1).stack, Chips{300});
    REQUIRE_EQ(player(table.snapshot(), 2).stack, Chips{200});
    REQUIRE_EQ(player(table.snapshot(), 3).stack, Chips{100});
    REQUIRE_EQ(table.totalChips(), Chips{600});
}

void shortAllInDoesNotReopenRaise() {
    Table table(config());
    seatAndReady(table, 1, 0, 1'000);
    seatAndReady(table, 2, 1, 150);
    seatAndReady(table, 3, 2, 1'000);
    REQUIRE_EQ(table.startHandWithDeck(deckWithPrefix({})), TableError::ok);

    REQUIRE(table.act({1, ActionType::raise, 100}));
    REQUIRE(table.act({2, ActionType::all_in, 0}));
    REQUIRE(table.act({3, ActionType::call, 0}));
    REQUIRE_EQ(table.snapshot().acting_player, std::optional<PlayerId>{1});
    REQUIRE_EQ(table.act({1, ActionType::raise, 300}).error, TableError::action_not_allowed);
    REQUIRE(table.act({1, ActionType::call, 0}));
    REQUIRE_EQ(table.snapshot().street, Street::flop);
}

void splitPotOddChipGoesLeftOfDealer() {
    TableConfig cfg = config(100, 1'000);
    cfg.small_blind = 1;
    cfg.big_blind = 2;
    Table table(cfg);
    seatAndReady(table, 1, 0, 101);
    seatAndReady(table, 2, 1, 101);
    seatAndReady(table, 3, 2, 101);

    const auto deck = deckWithPrefix({
        c(Rank::ten, Suit::clubs), c(Rank::nine, Suit::clubs), c(Rank::ten, Suit::diamonds),
        c(Rank::four, Suit::clubs), c(Rank::nine, Suit::diamonds), c(Rank::three, Suit::clubs),
        c(Rank::six, Suit::clubs), c(Rank::ace, Suit::spades), c(Rank::king, Suit::hearts),
        c(Rank::queen, Suit::diamonds), c(Rank::six, Suit::diamonds), c(Rank::jack, Suit::spades),
        c(Rank::seven, Suit::clubs), c(Rank::two, Suit::clubs),
    });
    REQUIRE_EQ(table.startHandWithDeck(deck), TableError::ok);
    REQUIRE(table.act({1, ActionType::all_in, 0}));
    REQUIRE(table.act({2, ActionType::call, 0}));
    REQUIRE(table.act({3, ActionType::call, 0}));

    REQUIRE_EQ(table.snapshot().street, Street::settled);
    REQUIRE_EQ(table.lastAwards().size(), std::size_t{1});
    REQUIRE_EQ(table.lastAwards()[0].pot_size, Chips{303});
    REQUIRE_EQ(table.lastAwards()[0].winners, (std::vector<PlayerId>{2, 1}));
    REQUIRE_EQ(table.lastAwards()[0].equal_share, Chips{151});
    REQUIRE_EQ(table.lastAwards()[0].odd_chips, Chips{1});
    REQUIRE_EQ(player(table.snapshot(), 1).stack, Chips{151});
    REQUIRE_EQ(player(table.snapshot(), 2).stack, Chips{152});
    REQUIRE_EQ(player(table.snapshot(), 3).stack, Chips{0});
    REQUIRE_EQ(table.totalChips(), Chips{303});
}

void randomizedHandsPreserveCardsTurnsAndChips() {
    for (std::uint64_t seed = 1; seed <= 1'000; ++seed) {
        Table table(config());
        const auto player_count = static_cast<std::size_t>(2 + seed % 5);
        for (std::size_t seat = 0; seat < player_count; ++seat) {
            seatAndReady(table,
                         static_cast<PlayerId>(seat + 1),
                         seat,
                         static_cast<Chips>(500 + ((seed + seat * 97) % 1'500)));
        }
        const auto expected_total = table.totalChips();
        poker::domain::DeterministicRandomSource random(seed);
        REQUIRE_EQ(table.startHand(random), TableError::ok);

        std::size_t actions = 0;
        while (table.snapshot().street != Street::settled) {
            const auto before = table.snapshot();
            REQUIRE(before.acting_player.has_value());
            const auto& actor = player(before, *before.acting_player);
            ActionType action = actor.street_commitment < before.current_bet
                                    ? ActionType::call
                                    : ActionType::check;
            if ((seed + actions * 13) % 17 == 0 && before.players.size() > 2) {
                action = ActionType::fold;
            }
            const auto result = table.act({actor.id, action, 0});
            REQUIRE(result);
            REQUIRE_EQ(table.totalChips(), expected_total);
            REQUIRE(++actions < 100);
        }

        const auto final = table.snapshot();
        std::array<bool, 52> visible{};
        for (const auto card : final.board) {
            const auto index = poker::domain::cardIndex(card);
            REQUIRE(!visible[index]);
            visible[index] = true;
        }
        for (const auto& final_player : final.players) {
            for (const auto card : final_player.hole_cards) {
                const auto index = poker::domain::cardIndex(card);
                REQUIRE(!visible[index]);
                visible[index] = true;
            }
        }
        REQUIRE_EQ(table.totalChips(), expected_total);
    }
}

void lengthFieldCodecHandlesTcpStreamBoundaries() {
    using poker::net::CodecError;
    using poker::net::LengthFieldCodec;

    const auto first = LengthFieldCodec::encode("first payload");
    const auto second = LengthFieldCodec::encode("second");
    std::vector<std::uint8_t> stream = first;
    stream.insert(stream.end(), second.begin(), second.end());

    LengthFieldCodec codec;
    std::vector<std::string> decoded;
    for (const auto byte : stream) {
        const auto result = codec.feed(&byte, 1);
        REQUIRE(result);
        for (const auto& frame : result.frames) {
            decoded.emplace_back(frame.begin(), frame.end());
        }
    }
    REQUIRE_EQ(decoded, (std::vector<std::string>{"first payload", "second"}));
    REQUIRE_EQ(codec.bufferedBytes(), std::size_t{0});

    LengthFieldCodec coalesced;
    const auto all = coalesced.feed(stream);
    REQUIRE(all);
    REQUIRE_EQ(all.frames.size(), std::size_t{2});

    LengthFieldCodec limited(16);
    const std::vector<std::uint8_t> oversized_header{0, 0, 0, 17};
    REQUIRE_EQ(limited.feed(oversized_header).error, CodecError::frame_too_large);
    REQUIRE_EQ(limited.feed(first).error, CodecError::decoder_failed);
    limited.reset();
    REQUIRE(limited.feed(first));

    LengthFieldCodec empty;
    REQUIRE_EQ(empty.feed(std::vector<std::uint8_t>{0, 0, 0, 0}).error, CodecError::empty_frame);

    LengthFieldCodec bounded(8);
    REQUIRE_EQ(bounded.feed(std::vector<std::uint8_t>(13, 1)).error,
               CodecError::frame_too_large);
}

void idempotencyRejectsDuplicatesAndSequenceGaps() {
    using poker::application::IdempotencyCache;
    using poker::application::RequestStatus;

    IdempotencyCache cache(2);
    REQUIRE_EQ(cache.begin(7, 1001, 1).status, RequestStatus::accepted);
    REQUIRE_EQ(cache.begin(7, 1001, 1).status, RequestStatus::in_flight);
    REQUIRE(cache.complete(7, 1001, {1, 2, 3}));
    const auto duplicate = cache.begin(7, 1001, 1);
    REQUIRE_EQ(duplicate.status, RequestStatus::duplicate);
    REQUIRE_EQ(*duplicate.cached_response, (std::vector<std::uint8_t>{1, 2, 3}));

    REQUIRE_EQ(cache.begin(7, 1003, 3).status, RequestStatus::sequence_gap);
    REQUIRE_EQ(cache.begin(7, 1002, 2).status, RequestStatus::accepted);
    REQUIRE(cache.complete(7, 1002, {4}));
    REQUIRE_EQ(cache.begin(7, 1004, 3).status, RequestStatus::accepted);
    REQUIRE(cache.complete(7, 1004, {5}));
    REQUIRE_EQ(cache.begin(7, 1001, 1).status, RequestStatus::stale_sequence);
    REQUIRE_EQ(cache.userCount(), std::size_t{1});
    cache.reset(7);
    REQUIRE_EQ(cache.userCount(), std::size_t{0});

    IdempotencyCache saturated(2);
    REQUIRE_EQ(saturated.begin(9, 2001, 1).status, RequestStatus::accepted);
    REQUIRE_EQ(saturated.begin(9, 2002, 2).status, RequestStatus::accepted);
    REQUIRE_EQ(saturated.begin(9, 2003, 3).status, RequestStatus::capacity_exceeded);
    REQUIRE(saturated.complete(9, 2001, {1}));
    REQUIRE_EQ(saturated.begin(9, 2003, 3).status, RequestStatus::accepted);
}

void roomExecutorSerializesEachTableAndSurvivesTaskFailure() {
    using poker::application::RoomExecutor;

    std::atomic<int> exceptions{0};
    RoomExecutor executor(4, 1024, [&exceptions](std::exception_ptr) { ++exceptions; });
    std::mutex mutex;
    std::condition_variable done;
    std::vector<int> observed;
    int completed = 0;

    for (int value = 0; value < 100; ++value) {
        REQUIRE(executor.post(42, [&, value] {
            observed.push_back(value);
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++completed;
            }
            done.notify_one();
        }));
    }
    REQUIRE(executor.post(42, [] { throw std::runtime_error("expected test exception"); }));
    REQUIRE(executor.post(42, [&] {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++completed;
        }
        done.notify_one();
    }));

    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(done.wait_for(lock, std::chrono::seconds(5), [&] { return completed == 101; }));
    }
    executor.stop();
    REQUIRE_EQ(observed.size(), std::size_t{100});
    for (int value = 0; value < 100; ++value) {
        REQUIRE_EQ(observed[static_cast<std::size_t>(value)], value);
    }
    REQUIRE_EQ(exceptions.load(), 1);
}

class TestCrypto final : public poker::security::CryptoProvider {
public:
    std::string hashPassword(std::string_view password) override {
        return "password-hash:" + std::string(password);
    }

    bool verifyPassword(std::string_view password, std::string_view encoded_hash) override {
        return encoded_hash == hashPassword(password);
    }

    std::string generateSessionToken() override {
        return "raw-token-" + std::to_string(++token_);
    }

    std::string hashSessionToken(std::string_view raw_token) override {
        return "token-hash:" + std::string(raw_token);
    }

private:
    int token_{0};
};

void authenticationNeverStoresOrReturnsPlaintextPasswords() {
    using poker::security::AuthError;

    poker::storage::InMemoryAccountStore store;
    TestCrypto crypto;
    poker::security::AuthService auth(store, crypto, 60, 100'000);

    REQUIRE_EQ(auth.registerUser("x", "long-password").error, AuthError::invalid_username);
    REQUIRE_EQ(auth.registerUser("alice", "short").error, AuthError::weak_password);
    const auto registered = auth.registerUser("alice_1", "correct-horse");
    REQUIRE(registered);
    REQUIRE_EQ(registered.value->wallet_balance, std::int64_t{100'000});
    REQUIRE(registered.value->password_hash != "correct-horse");
    REQUIRE_EQ(auth.registerUser("alice_1", "another-password").error, AuthError::username_taken);
    REQUIRE_EQ(auth.login("alice_1", "wrong-password", 1'000).error, AuthError::invalid_credentials);

    const auto login = auth.login("alice_1", "correct-horse", 1'000);
    REQUIRE(login);
    REQUIRE_EQ(login.value->expires_at_unix_ms, std::int64_t{61'000});
    REQUIRE(!login.value->raw_token.empty());
    const auto authenticated = auth.authenticate(login.value->raw_token, 2'000);
    REQUIRE(authenticated);
    REQUIRE_EQ(authenticated.value->user_id, registered.value->id);
    REQUIRE_EQ(store.updateSessionSequence(
                   crypto.hashSessionToken(login.value->raw_token), 0, 1),
               poker::storage::StorageError::ok);
    const auto refreshed = auth.refresh(login.value->raw_token, 2'000);
    REQUIRE(refreshed);
    REQUIRE(refreshed.value->raw_token != login.value->raw_token);
    REQUIRE_EQ(refreshed.value->expires_at_unix_ms, std::int64_t{62'000});
    REQUIRE_EQ(refreshed.value->last_client_sequence, std::uint64_t{1});
    REQUIRE_EQ(auth.authenticate(login.value->raw_token, 2'001).error, AuthError::invalid_session);
    REQUIRE(auth.authenticate(refreshed.value->raw_token, 2'001));
    REQUIRE_EQ(auth.logout(refreshed.value->raw_token), AuthError::ok);
    REQUIRE_EQ(auth.authenticate(refreshed.value->raw_token, 2'002).error,
               AuthError::invalid_session);

    const auto expiring = auth.login("alice_1", "correct-horse", 1'000);
    REQUIRE(expiring);
    REQUIRE(auth.authenticate(expiring.value->raw_token, 60'999));
    REQUIRE_EQ(auth.authenticate(expiring.value->raw_token, 61'000).error,
               AuthError::invalid_session);
}

void lobbyRoutesOnlyToHealthyLowLoadNodesAndUsesOneTimeTickets() {
    std::int64_t now = 1'000;
    poker::cluster::InMemoryNodeRegistry registry([&now] { return now; });
    TestCrypto crypto;
    poker::cluster::LobbyRouter router(registry, crypto, 60, 10);

    REQUIRE(registry.ping());
    REQUIRE(registry.heartbeat({"node-a", "10.0.0.1:7000", 10, 100}, 5));
    REQUIRE(registry.heartbeat({"node-b", "10.0.0.2:7000", 2, 500}, 5));
    REQUIRE_EQ(registry.allocateTableId(), std::optional<std::uint64_t>{1'000});
    REQUIRE_EQ(registry.allocateTableId(), std::optional<std::uint64_t>{1'001});
    const auto assignment = router.assignNewTable(99, 7);
    REQUIRE(assignment.has_value());
    REQUIRE_EQ(assignment->node.node_id, std::string{"node-b"});
    REQUIRE_EQ(registry.roomOwner(99), std::optional<std::string>{"node-b"});

    const auto ticket_hash = crypto.hashSessionToken(assignment->join_ticket);
    const auto ticket = registry.consumeJoinTicket(ticket_hash);
    REQUIRE(ticket.has_value());
    REQUIRE_EQ(ticket->user_id, std::uint64_t{7});
    REQUIRE_EQ(ticket->table_id, std::uint64_t{99});
    REQUIRE(!registry.consumeJoinTicket(ticket_hash).has_value());

    now += 5'001;
    REQUIRE(!registry.roomOwner(99).has_value());
    REQUIRE_EQ(registry.healthyNodes().size(), std::size_t{0});
    REQUIRE(!router.routeExistingTable(99, 8).has_value());

    REQUIRE(registry.heartbeat({"node-b", "10.0.0.2:7000", 2, 500}, 5));
    const auto recovered = router.routeExistingTable(99, 8, "node-b");
    REQUIRE(recovered.has_value());
    REQUIRE_EQ(recovered->node.node_id, std::string{"node-b"});
    REQUIRE_EQ(registry.roomOwner(99), std::optional<std::string>{"node-b"});

    poker::cluster::InMemoryNodeRegistry balanced_registry([&now] { return now; });
    poker::cluster::LobbyRouter balanced_router(balanced_registry, crypto, 60, 10);
    REQUIRE(balanced_registry.heartbeat({"node-a", "10.0.0.1:7000", 0, 0}, 5));
    REQUIRE(balanced_registry.heartbeat({"node-b", "10.0.0.2:7000", 0, 0}, 5));
    const auto first_balanced = balanced_router.assignNewTable(100, 7);
    const auto second_balanced = balanced_router.assignNewTable(101, 7);
    REQUIRE(first_balanced.has_value());
    REQUIRE(second_balanced.has_value());
    REQUIRE_EQ(first_balanced->node.node_id, std::string{"node-a"});
    REQUIRE_EQ(second_balanced->node.node_id, std::string{"node-b"});
}

void roomManagerProvidesSerializedReconnectAndTimeoutOperations() {
    using poker::application::RoomManager;

    RoomManager manager(2);
    REQUIRE_EQ(manager.createTable(88, config()), TableError::ok);
    REQUIRE_EQ(manager.tableIds(), (std::vector<std::uint64_t>{88}));

    const auto waitError = [](auto submit) {
        std::promise<TableError> promise;
        auto future = promise.get_future();
        REQUIRE(submit([&promise](TableError error) { promise.set_value(error); }));
        REQUIRE(future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        return future.get();
    };

    REQUIRE_EQ(waitError([&](RoomManager::ErrorCallback callback) {
                   return manager.seatPlayer(88, 1, 0, 1'000, std::move(callback));
               }),
               TableError::ok);
    REQUIRE_EQ(waitError([&](RoomManager::ErrorCallback callback) {
                   return manager.seatPlayer(88, 2, 1, 1'000, std::move(callback));
               }),
               TableError::ok);
    REQUIRE_EQ(waitError([&](RoomManager::ErrorCallback callback) {
                   return manager.setReady(88, 1, true, std::move(callback));
               }),
               TableError::ok);
    REQUIRE_EQ(waitError([&](RoomManager::ErrorCallback callback) {
                   return manager.setReady(88, 2, true, std::move(callback));
               }),
               TableError::ok);
    REQUIRE_EQ(waitError([&](RoomManager::ErrorCallback callback) {
                   return manager.startHandWithDeck(88, deckWithPrefix({}), std::move(callback));
               }),
               TableError::ok);

    std::promise<poker::domain::TableSnapshot> disconnected_promise;
    auto disconnected_future = disconnected_promise.get_future();
    REQUIRE(manager.setConnected(88, 1, false,
                                 [&disconnected_promise](TableError error, auto snapshot) {
                                     REQUIRE_EQ(error, TableError::ok);
                                     disconnected_promise.set_value(std::move(snapshot));
                                 }));
    REQUIRE(disconnected_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    const auto disconnected = disconnected_future.get();
    REQUIRE(!player(disconnected, 1).connected);
    REQUIRE_EQ(player(disconnected, 1).hole_cards.size(), std::size_t{2});
    REQUIRE_EQ(player(disconnected, 2).hole_cards.size(), std::size_t{0});

    std::promise<poker::domain::ActionResult> timeout_promise;
    auto timeout_future = timeout_promise.get_future();
    REQUIRE(manager.onActionTimeout(88, 1, [&timeout_promise](auto result, auto, auto) {
        timeout_promise.set_value(std::move(result));
    }));
    REQUIRE(timeout_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE(timeout_future.get());

    std::promise<poker::domain::TableSnapshot> snapshot_promise;
    auto snapshot_future = snapshot_promise.get_future();
    REQUIRE(manager.snapshot(88, 2, [&snapshot_promise](TableError error, auto snapshot) {
        REQUIRE_EQ(error, TableError::ok);
        snapshot_promise.set_value(std::move(snapshot));
    }));
    REQUIRE(snapshot_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE_EQ(snapshot_future.get().street, Street::settled);
    manager.stop();
}

void closedRoomsRejectCommandsAlreadyWaitingInTheShardQueue() {
    using poker::application::RoomManager;

    RoomManager manager(1);
    REQUIRE_EQ(manager.createTable(89, config()), TableError::ok);

    std::mutex gate_mutex;
    std::condition_variable gate;
    bool first_callback_entered = false;
    bool release_first_callback = false;
    std::promise<TableError> first_promise;
    auto first_future = first_promise.get_future();
    REQUIRE(manager.seatPlayer(89, 1, 0, 1'000,
                               [&](TableError error) {
        std::unique_lock<std::mutex> lock(gate_mutex);
        first_callback_entered = true;
        gate.notify_one();
        gate.wait(lock, [&] { return release_first_callback; });
        first_promise.set_value(error);
    }));

    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate.wait_for(lock, std::chrono::seconds(2), [&] {
            return first_callback_entered;
        }));
    }

    std::promise<TableError> queued_promise;
    auto queued_future = queued_promise.get_future();
    REQUIRE(manager.seatPlayer(89, 2, 1, 1'000,
                               [&queued_promise](TableError error) {
        queued_promise.set_value(error);
    }));
    REQUIRE(manager.closeTable(89));
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_first_callback = true;
    }
    gate.notify_one();

    REQUIRE(first_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE_EQ(first_future.get(), TableError::ok);
    REQUIRE(queued_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    REQUIRE_EQ(queued_future.get(), TableError::service_unavailable);
    REQUIRE(!manager.hasTable(89));
    manager.stop();
}

void gameStorePreservesWalletEscrowAndSettlementIdempotency() {
    using poker::storage::HandActionRecord;
    using poker::storage::HandPlayerSettlement;
    using poker::storage::HandSettlementRecord;
    using poker::storage::HandStartPlayer;
    using poker::storage::HandStartRecord;
    using poker::storage::InMemoryGameStore;
    using poker::storage::StorageError;
    using poker::storage::TableRecord;

    InMemoryGameStore store;
    REQUIRE_EQ(store.seedWallet(1, 1'000), StorageError::ok);
    REQUIRE_EQ(store.seedWallet(2, 1'000), StorageError::ok);
    REQUIRE_EQ(store.createTable(TableRecord{77, "settlement", "node-a", config(), 1}),
               StorageError::ok);
    REQUIRE_EQ(store.createTable(TableRecord{78, "refund", "node-a", config(), 1}),
               StorageError::ok);

    const auto buy_in = store.reserveBuyIn(77, 1, 0, 300, "buy:77:1");
    REQUIRE(buy_in);
    REQUIRE_EQ(buy_in.value->wallet_balance, std::int64_t{700});
    REQUIRE_EQ(store.reserveBuyIn(77, 1, 0, 300, "buy:77:1").value->table_stack,
               std::int64_t{300});
    REQUIRE_EQ(store.reserveBuyIn(77, 1, 0, 400, "buy:77:1").error,
               StorageError::conflict);
    REQUIRE_EQ(store.reserveBuyIn(77, 1, 1, 300, "buy:77:1").error,
               StorageError::conflict);
    REQUIRE_EQ(store.walletBalance(1), std::optional<std::int64_t>{700});
    REQUIRE_EQ(store.cashOut(77, 1, "buy:77:1").error, StorageError::conflict);
    REQUIRE(store.reserveBuyIn(77, 2, 1, 500, "buy:77:2"));

    REQUIRE_EQ(store.beginHand(HandStartRecord{
                   900,
                   77,
                   1,
                   0,
                   {HandStartPlayer{1, 0, 300,
                                    {c(Rank::ace, Suit::spades), c(Rank::ace, Suit::hearts)}},
                    HandStartPlayer{2, 1, 500,
                                    {c(Rank::king, Suit::spades), c(Rank::king, Suit::hearts)}}}}),
               StorageError::ok);
    REQUIRE_EQ(store.beginHand(HandStartRecord{900, 77, 1, 0, {}}),
               StorageError::invalid_data);
    REQUIRE_EQ(store.cashOut(77, 1, "cash:during-hand").error,
               StorageError::conflict);

    REQUIRE_EQ(store.appendAction(HandActionRecord{900, 1, 501, 1,
                                                   Street::preflop, ActionType::all_in, 300}),
               StorageError::ok);
    REQUIRE_EQ(store.appendAction(HandActionRecord{900, 1, 501, 1,
                                                   Street::preflop, ActionType::all_in, 300}),
               StorageError::duplicate);
    REQUIRE_EQ(store.appendAction(HandActionRecord{900, 1, 502, 2,
                                                   Street::preflop, ActionType::call, 300}),
               StorageError::conflict);
    REQUIRE_EQ(store.appendAction(HandActionRecord{900, 2, 501, 1,
                                                   Street::preflop, ActionType::all_in, 300}),
               StorageError::conflict);

    HandSettlementRecord settlement;
    settlement.hand_id = 900;
    settlement.table_id = 77;
    settlement.hand_number = 1;
    settlement.dealer_seat = 0;
    settlement.board = {c(Rank::ace, Suit::clubs), c(Rank::king, Suit::clubs),
                        c(Rank::queen, Suit::clubs), c(Rank::jack, Suit::clubs),
                        c(Rank::ten, Suit::clubs)};
    settlement.total_pot = 800;
    settlement.players = {
        HandPlayerSettlement{1, 0, 300, 300, 800, 800,
                             {c(Rank::ace, Suit::spades), c(Rank::ace, Suit::hearts)}, false},
        HandPlayerSettlement{2, 1, 500, 500, 0, 0,
                             {c(Rank::king, Suit::spades), c(Rank::king, Suit::hearts)}, false},
    };
    auto invalid_settlement = settlement;
    invalid_settlement.players[0].end_stack = 799;
    REQUIRE_EQ(store.settleHand(invalid_settlement), StorageError::invalid_data);
    REQUIRE_EQ(store.settleHand(settlement), StorageError::ok);
    REQUIRE_EQ(store.settleHand(settlement), StorageError::duplicate);
    auto conflicting_settlement = settlement;
    conflicting_settlement.players[0].folded = true;
    REQUIRE_EQ(store.settleHand(conflicting_settlement), StorageError::conflict);
    REQUIRE_EQ(store.appendAction(HandActionRecord{900, 3, 503, 2,
                                                   Street::river, ActionType::check, 0}),
               StorageError::conflict);
    REQUIRE_EQ(store.seatStack(77, 1), std::optional<std::int64_t>{800});
    REQUIRE_EQ(store.seatStack(77, 2), std::optional<std::int64_t>{0});
    REQUIRE(store.settlement(900).has_value());

    const auto cash_out = store.cashOut(77, 1, "cash:77:1");
    REQUIRE(cash_out);
    REQUIRE_EQ(cash_out.value->returned_stack, std::int64_t{800});
    REQUIRE_EQ(cash_out.value->wallet_balance, std::int64_t{1'500});
    REQUIRE_EQ(store.cashOut(77, 1, "cash:77:1").value->wallet_balance,
               std::int64_t{1'500});

    REQUIRE(store.reserveBuyIn(78, 2, 0, 300, "buy:78:2"));
    REQUIRE_EQ(store.walletBalance(2), std::optional<std::int64_t>{200});
    REQUIRE_EQ(store.abortTableAndRefund(78), StorageError::ok);
    REQUIRE_EQ(store.abortTableAndRefund(78), StorageError::ok);
    REQUIRE_EQ(store.walletBalance(2), std::optional<std::int64_t>{500});
    REQUIRE(!store.seatStack(78, 2).has_value());
}

void metricsExposeCountersGaugesAndCumulativeLatency() {
    poker::observability::MetricsRegistry metrics;
    metrics.connectionClosed();
    metrics.connectionOpened();
    REQUIRE_EQ(metrics.activeConnections(), std::uint64_t{1});
    metrics.requestReceived();
    metrics.responseSent();
    metrics.invalidFrame();
    metrics.rejectedRequest();
    metrics.actionAccepted();
    metrics.actionRejected();
    metrics.storageFailure();
    metrics.setTables(12);
    metrics.observeRequestLatency(std::chrono::microseconds(900));
    metrics.observeRequestLatency(std::chrono::microseconds(7'000));

    const auto output = metrics.renderPrometheus();
    REQUIRE(output.find("poker_connections 1\n") != std::string::npos);
    REQUIRE(output.find("poker_requests_total 1\n") != std::string::npos);
    REQUIRE(output.find("poker_tables 12\n") != std::string::npos);
    REQUIRE(output.find("poker_request_latency_seconds_bucket{le=\"0.001\"} 1\n")
            != std::string::npos);
    REQUIRE(output.find("poker_request_latency_seconds_bucket{le=\"0.01\"} 2\n")
            != std::string::npos);
    REQUIRE(output.find("poker_request_latency_seconds_count 2\n") != std::string::npos);
}

void tokenBucketEnforcesBurstAndRefillsOverTime() {
    using Bucket = poker::net::TokenBucket;
    Bucket bucket(2.0, 2);
    const auto start = Bucket::Clock::now();
    REQUIRE(bucket.allow(start));
    REQUIRE(bucket.allow(start));
    REQUIRE(!bucket.allow(start));
    REQUIRE(bucket.allow(start + std::chrono::milliseconds(500)));
    REQUIRE(!bucket.allow(start + std::chrono::milliseconds(500)));
    REQUIRE(bucket.allow(start + std::chrono::seconds(10)));
    REQUIRE(bucket.allow(start + std::chrono::seconds(10)));
    REQUIRE(!bucket.allow(start + std::chrono::seconds(10)));
}

void expiredGameNodeTriggersGracefulTableRefund() {
    using poker::storage::StorageError;
    std::int64_t now = 1'000;
    poker::cluster::InMemoryNodeRegistry registry([&now] { return now; });
    poker::storage::InMemoryGameStore games;
    REQUIRE(registry.heartbeat({"game-a", "127.0.0.1:7001", 1, 2}, 5));
    REQUIRE_EQ(games.seedWallet(1, 1'000), StorageError::ok);
    REQUIRE_EQ(games.createTable({50, "recover", "game-a", config(), 1}), StorageError::ok);
    REQUIRE(games.reserveBuyIn(50, 1, 0, 300, "buy:50:1"));
    REQUIRE_EQ(games.walletBalance(1), std::optional<std::int64_t>{700});

    poker::cluster::NodeFailureReaper reaper(
        registry, games, std::chrono::seconds(15), [&now] { return now; });
    now = 7'000;
    REQUIRE(reaper.sweep().refunded_tables.empty());
    REQUIRE(games.findTable(50));
    now = 21'999;
    REQUIRE(reaper.sweep().refunded_tables.empty());
    now = 22'000;
    const auto result = reaper.sweep();
    REQUIRE_EQ(result.refunded_tables, (std::vector<std::uint64_t>{50}));
    REQUIRE_EQ(games.walletBalance(1), std::optional<std::int64_t>{1'000});
    REQUIRE_EQ(games.findTable(50).error, StorageError::not_found);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"hand category examples", handCategoryExamples},
        {"exhaustive five-card distribution", exhaustiveFiveCardDistribution},
        {"heads-up order and fold settlement", headsUpOrderAndFoldSettlement},
        {"heads-up street order and showdown", headsUpStreetOrderAndShowdown},
        {"multi-level side pots", multiLevelSidePots},
        {"short all-in does not reopen raise", shortAllInDoesNotReopenRaise},
        {"split-pot odd chip", splitPotOddChipGoesLeftOfDealer},
        {"randomized hand invariants", randomizedHandsPreserveCardsTurnsAndChips},
        {"length-field codec", lengthFieldCodecHandlesTcpStreamBoundaries},
        {"idempotency and sequences", idempotencyRejectsDuplicatesAndSequenceGaps},
        {"room executor serialization", roomExecutorSerializesEachTableAndSurvivesTaskFailure},
        {"authentication service", authenticationNeverStoresOrReturnsPlaintextPasswords},
        {"lobby routing and tickets", lobbyRoutesOnlyToHealthyLowLoadNodesAndUsesOneTimeTickets},
        {"room manager reconnect and timeout", roomManagerProvidesSerializedReconnectAndTimeoutOperations},
        {"closed room rejects queued commands", closedRoomsRejectCommandsAlreadyWaitingInTheShardQueue},
        {"wallet escrow and settlement", gameStorePreservesWalletEscrowAndSettlementIdempotency},
        {"prometheus metrics", metricsExposeCountersGaugesAndCumulativeLatency},
        {"connection rate limiter", tokenBucketEnforcesBurstAndRefillsOverTime},
        {"failed-node refund grace period", expiredGameNodeTriggersGracefulTableRefund},
    };

    std::size_t failures = 0;
    for (const auto& [name, test] : tests) {
        const auto started = std::chrono::steady_clock::now();
        try {
            test();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            std::cout << "[PASS] " << name << " (" << elapsed.count() << " ms)\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << tests.size() << " test(s) passed\n";
    return 0;
}
