#ifndef POKER_STORAGE_GAME_STORE_HPP
#define POKER_STORAGE_GAME_STORE_HPP

#include "poker/domain/card.hpp"
#include "poker/domain/table.hpp"
#include "poker/storage/account_store.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace poker::storage {

struct TableRecord {
    std::uint64_t table_id{0};
    std::string name;
    std::string node_id;
    domain::TableConfig config;
    UserId created_by{0};
};

struct BuyInReceipt {
    std::int64_t wallet_balance{0};
    std::int64_t table_stack{0};
};

struct CashOutReceipt {
    std::int64_t wallet_balance{0};
    std::int64_t returned_stack{0};
};

struct HandActionRecord {
    std::uint64_t hand_id{0};
    std::uint64_t sequence{0};
    std::uint64_t request_id{0};
    UserId user_id{0};
    domain::Street street{domain::Street::preflop};
    domain::ActionType action{domain::ActionType::fold};
    std::int64_t target_amount{0};
};

struct HandStartPlayer {
    UserId user_id{0};
    std::uint32_t seat{0};
    std::int64_t start_stack{0};
    std::vector<domain::Card> hole_cards;
};

struct HandStartRecord {
    std::uint64_t hand_id{0};
    std::uint64_t table_id{0};
    std::uint64_t hand_number{0};
    std::uint32_t dealer_seat{0};
    std::vector<HandStartPlayer> players;
};

struct HandPlayerSettlement {
    UserId user_id{0};
    std::uint32_t seat{0};
    std::int64_t start_stack{0};
    std::int64_t committed{0};
    std::int64_t winnings{0};
    std::int64_t end_stack{0};
    std::vector<domain::Card> hole_cards;
    bool folded{false};
};

struct HandSettlementRecord {
    std::uint64_t hand_id{0};
    std::uint64_t table_id{0};
    std::uint64_t hand_number{0};
    std::uint32_t dealer_seat{0};
    std::vector<domain::Card> board;
    std::int64_t total_pot{0};
    std::vector<HandPlayerSettlement> players;
};

class GameStore {
public:
    virtual ~GameStore() = default;

    virtual StorageResult<TableRecord> findTable(std::uint64_t table_id) = 0;
    virtual StorageResult<std::vector<TableRecord>> listOpenTables(std::size_t limit) = 0;
    virtual StorageResult<std::size_t> countOpenTables(std::string_view node_id) = 0;
    virtual StorageError createTable(const TableRecord& table) = 0;
    virtual StorageResult<BuyInReceipt> reserveBuyIn(std::uint64_t table_id,
                                                      UserId user_id,
                                                      std::uint32_t seat,
                                                      std::int64_t amount,
                                                      std::string idempotency_key) = 0;
    virtual StorageResult<CashOutReceipt> cashOut(std::uint64_t table_id,
                                                  UserId user_id,
                                                  std::string idempotency_key) = 0;
    virtual StorageError beginHand(const HandStartRecord& hand) = 0;
    virtual StorageError appendAction(const HandActionRecord& action) = 0;
    virtual StorageError settleHand(const HandSettlementRecord& settlement) = 0;
    virtual StorageError abortTableAndRefund(std::uint64_t table_id) = 0;
};

}  // namespace poker::storage

#endif
