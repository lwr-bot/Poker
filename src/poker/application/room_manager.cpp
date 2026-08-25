#include "poker/application/room_manager.hpp"

#include <algorithm>
#include <utility>

namespace poker::application {

RoomManager::RoomManager(std::size_t logic_shards, std::size_t max_pending_per_shard)
    : executor_(logic_shards, max_pending_per_shard) {}

domain::TableError RoomManager::createTable(TableId table_id, domain::TableConfig config) {
    if (table_id == 0) {
        return domain::TableError::invalid_configuration;
    }
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    if (rooms_.count(table_id) > 0) {
        return domain::TableError::seat_occupied;
    }
    rooms_.emplace(table_id, std::make_shared<Room>(table_id, config));
    return domain::TableError::ok;
}

bool RoomManager::closeTable(TableId table_id) {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    const auto found = rooms_.find(table_id);
    if (found == rooms_.end()) {
        return false;
    }
    found->second->closed.store(true, std::memory_order_release);
    rooms_.erase(found);
    return true;
}

bool RoomManager::hasTable(TableId table_id) const {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    return rooms_.count(table_id) != 0;
}

std::size_t RoomManager::tableCount() const {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    return rooms_.size();
}

std::vector<TableId> RoomManager::tableIds() const {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    std::vector<TableId> result;
    result.reserve(rooms_.size());
    for (const auto& entry : rooms_) {
        result.push_back(entry.first);
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool RoomManager::seatPlayer(TableId table_id,
                             domain::PlayerId player_id,
                             std::size_t seat,
                             domain::Chips buy_in,
                             ErrorCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, player_id, seat, buy_in, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable);
                return;
            }
            if (callback) {
                callback(room->table.seatPlayer(player_id, seat, buy_in));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable);
        }
        return false;
    }
    return true;
}

bool RoomManager::removePlayer(TableId table_id,
                               domain::PlayerId player_id,
                               ErrorCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, player_id, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable);
                return;
            }
            if (callback) {
                callback(room->table.removePlayer(player_id));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable);
        }
        return false;
    }
    return true;
}

bool RoomManager::setReady(TableId table_id,
                           domain::PlayerId player_id,
                           bool ready,
                           ErrorCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, player_id, ready, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable);
                return;
            }
            if (callback) {
                callback(room->table.setReady(player_id, ready));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable);
        }
        return false;
    }
    return true;
}

bool RoomManager::startHand(TableId table_id, ErrorCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable);
                return;
            }
            domain::SystemRandomSource random;
            if (callback) {
                callback(room->table.startHand(random));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable);
        }
        return false;
    }
    return true;
}

bool RoomManager::startHandWithDeck(TableId table_id,
                                    std::vector<domain::Card> cards,
                                    ErrorCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    auto cards_ptr = std::make_shared<std::vector<domain::Card>>(std::move(cards));
    if (!executor_.post(table_id, [room, cards_ptr, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable);
                return;
            }
            if (callback) {
                callback(room->table.startHandWithDeck(std::move(*cards_ptr)));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable);
        }
        return false;
    }
    return true;
}

bool RoomManager::abortHand(TableId table_id, ErrorCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable);
                return;
            }
            if (callback) {
                callback(room->table.abortHand());
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable);
        }
        return false;
    }
    return true;
}

bool RoomManager::act(TableId table_id,
                      domain::ActionCommand command,
                      ActionCallback callback) {
    return act(table_id, 0, command, std::move(callback));
}

bool RoomManager::act(TableId table_id,
                      std::uint64_t expected_hand_id,
                      domain::ActionCommand command,
                      ActionCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, expected_hand_id, command, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) {
                    callback({domain::TableError::service_unavailable,
                              "table is closed",
                              0,
                              {}},
                             {},
                             {});
                }
                return;
            }
            domain::ActionResult result;
            const auto before = room->table.snapshot(command.player_id);
            if (expected_hand_id != 0 && before.hand_id != expected_hand_id) {
                result = {domain::TableError::action_not_allowed,
                          "action belongs to a stale hand",
                          before.server_sequence,
                          {}};
            } else {
                result = room->table.act(command);
            }
            auto view = room->table.snapshot(command.player_id);
            auto audit = room->table.auditSnapshot();
            if (callback) {
                callback(std::move(result), std::move(view), std::move(audit));
            }
        })) {
        if (callback) {
            callback({domain::TableError::service_unavailable,
                      "room executor is overloaded",
                      0,
                      {}},
                     {},
                     {});
        }
        return false;
    }
    return true;
}

bool RoomManager::setConnected(TableId table_id,
                               domain::PlayerId player_id,
                               bool connected,
                               SnapshotCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, player_id, connected, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable, {});
                return;
            }
            const auto error = room->table.setConnected(player_id, connected);
            auto view = room->table.snapshot(player_id);
            if (callback) {
                callback(error, std::move(view));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable, {});
        }
        return false;
    }
    return true;
}

bool RoomManager::snapshot(TableId table_id,
                           std::optional<domain::PlayerId> viewer,
                           SnapshotCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, viewer, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable, {});
                return;
            }
            if (callback) {
                callback(domain::TableError::ok, room->table.snapshot(viewer));
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable, {});
        }
        return false;
    }
    return true;
}

bool RoomManager::auditSnapshot(TableId table_id, SnapshotCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) callback(domain::TableError::service_unavailable, {});
                return;
            }
            if (callback) {
                callback(domain::TableError::ok, room->table.auditSnapshot());
            }
        })) {
        if (callback) {
            callback(domain::TableError::service_unavailable, {});
        }
        return false;
    }
    return true;
}

bool RoomManager::onActionTimeout(TableId table_id,
                                  domain::PlayerId player_id,
                                  ActionCallback callback) {
    return onActionTimeout(table_id, player_id, 0, std::move(callback));
}

bool RoomManager::onActionTimeout(TableId table_id,
                                  domain::PlayerId player_id,
                                  std::uint64_t expected_server_sequence,
                                  ActionCallback callback) {
    const auto room = findRoom(table_id);
    if (!room) {
        return rejectMissingRoom(callback);
    }
    if (!executor_.post(table_id, [room, player_id, expected_server_sequence, callback] {
            if (room->closed.load(std::memory_order_acquire)) {
                if (callback) {
                    callback({domain::TableError::service_unavailable,
                              "table is closed",
                              0,
                              {}},
                             {},
                             {});
                }
                return;
            }
            const auto before = room->table.snapshot(player_id);
            domain::ActionResult result;
            if ((expected_server_sequence != 0
                 && before.server_sequence != expected_server_sequence)
                || !before.acting_player.has_value() || *before.acting_player != player_id) {
                result = {domain::TableError::not_players_turn,
                          "timeout is stale or no longer belongs to this player",
                          before.server_sequence,
                          {}};
            } else {
                const auto found = std::find_if(before.players.begin(), before.players.end(),
                                                [player_id](const auto& value) {
                                                    return value.id == player_id;
                                                });
                const bool can_check = found != before.players.end()
                                       && found->street_commitment == before.current_bet;
                result = room->table.act({player_id,
                                          can_check ? domain::ActionType::check
                                                    : domain::ActionType::fold,
                                          0});
            }
            auto after = room->table.snapshot(player_id);
            auto audit = room->table.auditSnapshot();
            if (callback) {
                callback(std::move(result), std::move(after), std::move(audit));
            }
        })) {
        if (callback) {
            callback({domain::TableError::service_unavailable,
                      "room executor is overloaded",
                      0,
                      {}},
                     {},
                     {});
        }
        return false;
    }
    return true;
}

void RoomManager::stop() {
    executor_.stop();
}

std::shared_ptr<RoomManager::Room> RoomManager::findRoom(TableId table_id) const {
    std::lock_guard<std::mutex> lock(rooms_mutex_);
    const auto found = rooms_.find(table_id);
    return found == rooms_.end() ? nullptr : found->second;
}

bool RoomManager::rejectMissingRoom(const ErrorCallback& callback) const {
    if (callback) {
        callback(domain::TableError::player_not_found);
    }
    return false;
}

bool RoomManager::rejectMissingRoom(const SnapshotCallback& callback) const {
    if (callback) {
        callback(domain::TableError::player_not_found, {});
    }
    return false;
}

bool RoomManager::rejectMissingRoom(const ActionCallback& callback) const {
    if (callback) {
        callback({domain::TableError::player_not_found, "table does not exist", 0, {}}, {}, {});
    }
    return false;
}

}  // namespace poker::application
