#ifndef POKER_STORAGE_MYSQL_GAME_STORE_HPP
#define POKER_STORAGE_MYSQL_GAME_STORE_HPP

#include "poker/storage/game_store.hpp"
#include "poker/storage/mysql_connection_pool.hpp"

namespace poker::storage {

class MySqlGameStore final : public GameStore {
public:
    explicit MySqlGameStore(MySqlConnectionPool& pool);

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
    MySqlConnectionPool& pool_;
};

}  // namespace poker::storage

#endif
