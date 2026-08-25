#include "poker/storage/in_memory_game_store.hpp"
#include "poker/storage/game_store_validation.hpp"

#include <algorithm>
#include <numeric>
#include <utility>

namespace poker::storage {

StorageError InMemoryGameStore::seedWallet(UserId user_id, std::int64_t balance) {
    if (user_id == 0 || balance < 0) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (wallets_.count(user_id) != 0) {
        return StorageError::duplicate;
    }
    wallets_.emplace(user_id, balance);
    return StorageError::ok;
}

std::optional<std::int64_t> InMemoryGameStore::walletBalance(UserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = wallets_.find(user_id);
    return found == wallets_.end() ? std::nullopt
                                   : std::optional<std::int64_t>{found->second};
}

std::optional<std::int64_t> InMemoryGameStore::seatStack(std::uint64_t table_id,
                                                          UserId user_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto table = tables_.find(table_id);
    if (table == tables_.end()) {
        return std::nullopt;
    }
    const auto seat = table->second.seats.find(user_id);
    return seat == table->second.seats.end() ? std::nullopt
                                             : std::optional<std::int64_t>{seat->second.stack};
}

std::optional<HandSettlementRecord> InMemoryGameStore::settlement(
    std::uint64_t hand_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = settlements_.find(hand_id);
    return found == settlements_.end() ? std::nullopt
                                       : std::optional<HandSettlementRecord>{found->second};
}

StorageResult<TableRecord> InMemoryGameStore::findTable(std::uint64_t table_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tables_.find(table_id);
    if (found == tables_.end() || found->second.aborted) {
        return {StorageError::not_found, std::nullopt, "table not found"};
    }
    return {StorageError::ok, found->second.record, {}};
}

StorageResult<std::vector<TableRecord>> InMemoryGameStore::listOpenTables(std::size_t limit) {
    if (limit == 0) {
        return {StorageError::invalid_data, std::nullopt, "limit must be positive"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<TableRecord> result;
    for (const auto& [id, table] : tables_) {
        static_cast<void>(id);
        if (!table.aborted) {
            result.push_back(table.record);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.table_id < rhs.table_id;
    });
    if (result.size() > limit) {
        result.resize(limit);
    }
    return {StorageError::ok, std::move(result), {}};
}

StorageResult<std::size_t> InMemoryGameStore::countOpenTables(std::string_view node_id) {
    if (node_id.empty()) {
        return {StorageError::invalid_data, std::nullopt, "node id is required"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto count = static_cast<std::size_t>(std::count_if(
        tables_.begin(), tables_.end(), [node_id](const auto& entry) {
            return !entry.second.aborted && entry.second.record.node_id == node_id;
        }));
    return {StorageError::ok, count, {}};
}

StorageError InMemoryGameStore::createTable(const TableRecord& table) {
    if (table.table_id == 0 || table.created_by == 0 || table.name.empty()
        || table.node_id.empty() || table.config.min_players < 2
        || table.config.max_players < table.config.min_players
        || table.config.max_players > 6 || table.config.small_blind <= 0
        || table.config.big_blind < table.config.small_blind * 2
        || table.config.min_buy_in <= 0
        || table.config.max_buy_in < table.config.min_buy_in) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (tables_.count(table.table_id) != 0) {
        return StorageError::duplicate;
    }
    tables_.emplace(table.table_id, StoredTable{table, false, false, {}, {}});
    return StorageError::ok;
}

StorageResult<BuyInReceipt> InMemoryGameStore::reserveBuyIn(
    std::uint64_t table_id,
    UserId user_id,
    std::uint32_t seat,
    std::int64_t amount,
    std::string idempotency_key) {
    if (table_id == 0 || user_id == 0 || amount <= 0 || idempotency_key.empty()
        || idempotency_key.size() > 96) {
        return {StorageError::invalid_data, std::nullopt, "invalid buy-in"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = ledgerKey(user_id, idempotency_key);
    if (const auto replay = ledger_.find(key); replay != ledger_.end()) {
        if (replay->second.kind != LedgerKind::buy_in
            || replay->second.table_id != table_id || replay->second.chips != amount
            || replay->second.seat != seat) {
            return {StorageError::conflict, std::nullopt, "idempotency key was reused"};
        }
        const auto replay_table = tables_.find(table_id);
        if (replay_table == tables_.end()) {
            return {StorageError::conflict, std::nullopt,
                    "idempotent buy-in does not match an active table"};
        }
        const auto replay_seat = replay_table->second.seats.find(user_id);
        if (replay_seat == replay_table->second.seats.end() || replay_seat->second.seat != seat
            || replay_seat->second.stack != amount) {
            return {StorageError::conflict, std::nullopt,
                    "idempotent buy-in does not match the active seat"};
        }
        return {StorageError::ok,
                BuyInReceipt{replay->second.wallet_balance, replay->second.chips},
                {}};
    }
    const auto table = tables_.find(table_id);
    const auto wallet = wallets_.find(user_id);
    if (table == tables_.end() || table->second.aborted || wallet == wallets_.end()) {
        return {StorageError::not_found, std::nullopt, "table or wallet not found"};
    }
    if (table->second.hand_in_progress) {
        return {StorageError::conflict, std::nullopt, "buy-in is not allowed during a hand"};
    }
    const auto& config = table->second.record.config;
    if (seat >= config.max_players || amount < config.min_buy_in
        || amount > config.max_buy_in) {
        return {StorageError::invalid_data, std::nullopt, "invalid seat or buy-in amount"};
    }
    if (table->second.seats.count(user_id) != 0
        || table->second.occupied_seats.count(seat) != 0) {
        return {StorageError::conflict, std::nullopt, "player or seat is already reserved"};
    }
    if (wallet->second < amount) {
        return {StorageError::insufficient_funds, std::nullopt, "insufficient wallet balance"};
    }

    wallet->second -= amount;
    table->second.seats.emplace(user_id, SeatRecord{user_id, seat, amount});
    table->second.occupied_seats.emplace(seat, user_id);
    ledger_.emplace(std::move(key),
                    LedgerResult{LedgerKind::buy_in, table_id, wallet->second, amount, seat});
    return {StorageError::ok, BuyInReceipt{wallet->second, amount}, {}};
}

StorageResult<CashOutReceipt> InMemoryGameStore::cashOut(
    std::uint64_t table_id,
    UserId user_id,
    std::string idempotency_key) {
    if (table_id == 0 || user_id == 0 || idempotency_key.empty()
        || idempotency_key.size() > 96) {
        return {StorageError::invalid_data, std::nullopt, "invalid cash-out"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto key = ledgerKey(user_id, idempotency_key);
    if (const auto replay = ledger_.find(key); replay != ledger_.end()) {
        if (replay->second.kind != LedgerKind::cash_out
            || replay->second.table_id != table_id) {
            return {StorageError::conflict, std::nullopt, "idempotency key was reused"};
        }
        return {StorageError::ok,
                CashOutReceipt{replay->second.wallet_balance, replay->second.chips},
                {}};
    }
    const auto table = tables_.find(table_id);
    const auto wallet = wallets_.find(user_id);
    if (table == tables_.end() || wallet == wallets_.end()) {
        return {StorageError::not_found, std::nullopt, "table or wallet not found"};
    }
    if (table->second.hand_in_progress) {
        return {StorageError::conflict, std::nullopt, "cash-out is not allowed during a hand"};
    }
    const auto seat = table->second.seats.find(user_id);
    if (seat == table->second.seats.end()) {
        return {StorageError::not_found, std::nullopt, "seat not found"};
    }
    const auto chips = seat->second.stack;
    const auto seat_number = seat->second.seat;
    wallet->second += chips;
    table->second.occupied_seats.erase(seat_number);
    table->second.seats.erase(seat);
    ledger_.emplace(std::move(key),
                    LedgerResult{LedgerKind::cash_out, table_id, wallet->second, chips,
                                 seat_number});
    return {StorageError::ok, CashOutReceipt{wallet->second, chips}, {}};
}

StorageError InMemoryGameStore::beginHand(const HandStartRecord& hand) {
    if (hand.hand_id == 0 || hand.table_id == 0 || hand.hand_number == 0
        || hand.players.size() < 2) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (hand_starts_.count(hand.hand_id) != 0 || settlements_.count(hand.hand_id) != 0) {
        return StorageError::duplicate;
    }
    const auto table = tables_.find(hand.table_id);
    if (table == tables_.end() || table->second.aborted) {
        return StorageError::not_found;
    }
    if (table->second.hand_in_progress) {
        return StorageError::conflict;
    }
    for (const auto& player : hand.players) {
        const auto seat = table->second.seats.find(player.user_id);
        if (seat == table->second.seats.end() || seat->second.seat != player.seat
            || seat->second.stack != player.start_stack || player.hole_cards.size() != 2) {
            return StorageError::conflict;
        }
    }
    hand_starts_.emplace(hand.hand_id, hand);
    table->second.hand_in_progress = true;
    return StorageError::ok;
}

StorageError InMemoryGameStore::appendAction(const HandActionRecord& action) {
    if (action.hand_id == 0 || action.sequence == 0 || action.request_id == 0
        || action.user_id == 0) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (hand_starts_.count(action.hand_id) == 0) {
        return StorageError::not_found;
    }
    if (settlements_.count(action.hand_id) != 0) {
        return StorageError::conflict;
    }
    const auto same_action = [&action](const HandActionRecord& existing) {
        return existing.hand_id == action.hand_id && existing.sequence == action.sequence
               && existing.request_id == action.request_id
               && existing.user_id == action.user_id && existing.street == action.street
               && existing.action == action.action
               && existing.target_amount == action.target_amount;
    };
    const auto sequence_key = std::make_pair(action.hand_id, action.sequence);
    if (const auto existing = actions_.find(sequence_key); existing != actions_.end()) {
        return same_action(existing->second) ? StorageError::duplicate : StorageError::conflict;
    }
    const auto request_duplicate = std::find_if(
        actions_.begin(), actions_.end(), [&action](const auto& existing) {
            return existing.second.hand_id == action.hand_id
                   && existing.second.user_id == action.user_id
                   && existing.second.request_id == action.request_id;
        });
    if (request_duplicate != actions_.end()) {
        return same_action(request_duplicate->second) ? StorageError::duplicate
                                                       : StorageError::conflict;
    }
    actions_.emplace(sequence_key, action);
    return StorageError::ok;
}

StorageError InMemoryGameStore::settleHand(const HandSettlementRecord& value) {
    if (!validHandSettlement(value)) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto existing = settlements_.find(value.hand_id);
        existing != settlements_.end()) {
        return equivalentHandSettlement(existing->second, value) ? StorageError::duplicate
                                                                  : StorageError::conflict;
    }
    if (hand_starts_.count(value.hand_id) == 0) {
        return StorageError::not_found;
    }
    const auto table = tables_.find(value.table_id);
    if (table == tables_.end() || table->second.aborted) {
        return StorageError::not_found;
    }
    if (!table->second.hand_in_progress) {
        return StorageError::duplicate;
    }

    for (const auto& player : value.players) {
        const auto seat = table->second.seats.find(player.user_id);
        if (seat == table->second.seats.end() || seat->second.seat != player.seat
            || seat->second.stack != player.start_stack || player.committed < 0
            || player.winnings < 0 || player.end_stack < 0
            || player.end_stack != player.start_stack - player.committed + player.winnings) {
            return StorageError::conflict;
        }
    }
    for (const auto& player : value.players) {
        table->second.seats.at(player.user_id).stack = player.end_stack;
    }
    settlements_.emplace(value.hand_id, value);
    table->second.hand_in_progress = false;
    return StorageError::ok;
}

StorageError InMemoryGameStore::abortTableAndRefund(std::uint64_t table_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto table = tables_.find(table_id);
    if (table == tables_.end()) {
        return StorageError::not_found;
    }
    if (table->second.aborted) {
        return StorageError::ok;
    }
    for (const auto& [user_id, seat] : table->second.seats) {
        const auto wallet = wallets_.find(user_id);
        if (wallet == wallets_.end()) {
            return StorageError::conflict;
        }
        wallet->second += seat.stack;
    }
    table->second.seats.clear();
    table->second.occupied_seats.clear();
    table->second.aborted = true;
    table->second.hand_in_progress = false;
    return StorageError::ok;
}

std::string InMemoryGameStore::ledgerKey(UserId user_id,
                                          const std::string& idempotency_key) {
    return std::to_string(user_id) + ":" + idempotency_key;
}

}  // namespace poker::storage
