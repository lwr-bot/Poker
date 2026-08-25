#ifndef POKER_APPLICATION_ROOM_MANAGER_HPP
#define POKER_APPLICATION_ROOM_MANAGER_HPP

#include "poker/application/room_executor.hpp"
#include "poker/domain/table.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace poker::application {

class RoomManager {
public:
    using ErrorCallback = std::function<void(domain::TableError)>;
    using SnapshotCallback = std::function<void(domain::TableError, domain::TableSnapshot)>;
    using ActionCallback = std::function<void(domain::ActionResult,
                                              domain::TableSnapshot,
                                              domain::TableSnapshot)>;

    explicit RoomManager(std::size_t logic_shards = 4,
                         std::size_t max_pending_per_shard = 16'384);

    domain::TableError createTable(TableId table_id, domain::TableConfig config = {});
    bool closeTable(TableId table_id);
    bool hasTable(TableId table_id) const;
    std::size_t tableCount() const;
    std::vector<TableId> tableIds() const;

    bool seatPlayer(TableId table_id,
                    domain::PlayerId player_id,
                    std::size_t seat,
                    domain::Chips buy_in,
                    ErrorCallback callback);
    bool removePlayer(TableId table_id,
                      domain::PlayerId player_id,
                      ErrorCallback callback);
    bool setReady(TableId table_id,
                  domain::PlayerId player_id,
                  bool ready,
                  ErrorCallback callback);
    bool startHand(TableId table_id, ErrorCallback callback);
    bool startHandWithDeck(TableId table_id,
                           std::vector<domain::Card> cards,
                           ErrorCallback callback);
    bool abortHand(TableId table_id, ErrorCallback callback);
    bool act(TableId table_id, domain::ActionCommand command, ActionCallback callback);
    bool act(TableId table_id,
             std::uint64_t expected_hand_id,
             domain::ActionCommand command,
             ActionCallback callback);
    bool setConnected(TableId table_id,
                      domain::PlayerId player_id,
                      bool connected,
                      SnapshotCallback callback);
    bool snapshot(TableId table_id,
                  std::optional<domain::PlayerId> viewer,
                  SnapshotCallback callback);
    bool auditSnapshot(TableId table_id, SnapshotCallback callback);
    bool onActionTimeout(TableId table_id,
                         domain::PlayerId player_id,
                         ActionCallback callback);
    bool onActionTimeout(TableId table_id,
                         domain::PlayerId player_id,
                         std::uint64_t expected_server_sequence,
                         ActionCallback callback);

    void stop();

private:
    struct Room {
        Room(TableId value, domain::TableConfig config)
            : id(value), table(config) {}

        TableId id;
        std::atomic<bool> closed{false};
        domain::Table table;
    };

    std::shared_ptr<Room> findRoom(TableId table_id) const;
    bool rejectMissingRoom(const ErrorCallback& callback) const;
    bool rejectMissingRoom(const SnapshotCallback& callback) const;
    bool rejectMissingRoom(const ActionCallback& callback) const;

    mutable std::mutex rooms_mutex_;
    std::unordered_map<TableId, std::shared_ptr<Room>> rooms_;
    RoomExecutor executor_;
};

}  // namespace poker::application

#endif
