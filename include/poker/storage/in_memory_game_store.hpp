#ifndef POKER_STORAGE_IN_MEMORY_GAME_STORE_HPP
#define POKER_STORAGE_IN_MEMORY_GAME_STORE_HPP

#include "poker/storage/game_store.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace poker::storage {

// Deterministic transactional model used by tests. Production code uses
// MySqlGameStore, but both implementations share the same observable contract.
class InMemoryGameStore final : public GameStore {
public:
    StorageError seedWallet(UserId user_id, std::int64_t balance);
    std::optional<std::int64_t> walletBalance(UserId user_id) const;
    std::optional<std::int64_t> seatStack(std::uint64_t table_id, UserId user_id) const;
    std::optional<HandSettlementRecord> settlement(std::uint64_t hand_id) const;

    StorageResult<TableRecord> findTable(std::uint64_t table_id) override;
    StorageResult<std::vector<TableRecord>> listOpenTables(std::size_t limit) override;
    StorageResult<std::size_t> countOpenTables(std::string_view node_id) override;
    StorageError createTable(const TableRecord& table) override;
    StorageResult<BuyInReceipt> reserveBuyIn(std::uint64_t table_id,
                                              UserId user_id,
                                              std::uint32_t seat,
                                              std::int64_t amount,
                                              std::string idempotency_key) override;
    StorageResult<CashOutReceipt> cashOut(std::uint64_t table_id,
                                          UserId user_id,
                                          std::string idempotency_key) override;
    StorageError beginHand(const HandStartRecord& hand) override;
    StorageError appendAction(const HandActionRecord& action) override;
    StorageError settleHand(const HandSettlementRecord& settlement) override;
    StorageError abortTableAndRefund(std::uint64_t table_id) override;

private:
    struct SeatRecord {
        UserId user_id{0};
        std::uint32_t seat{0};
        std::int64_t stack{0};
    };

    struct StoredTable {
        TableRecord record;
        bool aborted{false};
        bool hand_in_progress{false};
        std::unordered_map<UserId, SeatRecord> seats;
        std::unordered_map<std::uint32_t, UserId> occupied_seats;
    };

    enum class LedgerKind : std::uint8_t { buy_in, cash_out };
    struct LedgerResult {
        LedgerKind kind{LedgerKind::buy_in};
        std::uint64_t table_id{0};
        std::int64_t wallet_balance{0};
        std::int64_t chips{0};
        std::uint32_t seat{0};
    };

    static std::string ledgerKey(UserId user_id, const std::string& idempotency_key);

    mutable std::mutex mutex_;
    std::unordered_map<UserId, std::int64_t> wallets_;
    std::unordered_map<std::uint64_t, StoredTable> tables_;
    std::unordered_map<std::string, LedgerResult> ledger_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, HandActionRecord> actions_;
    std::unordered_map<std::uint64_t, HandStartRecord> hand_starts_;
    std::unordered_map<std::uint64_t, HandSettlementRecord> settlements_;
};

}  // namespace poker::storage

#endif
