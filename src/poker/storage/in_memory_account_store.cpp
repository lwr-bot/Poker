#include "poker/storage/in_memory_account_store.hpp"

#include <utility>

namespace poker::storage {

StorageResult<UserRecord> InMemoryAccountStore::createUser(std::string username,
                                                            std::string password_hash,
                                                            std::int64_t initial_chips) {
    if (username.empty() || password_hash.empty() || initial_chips < 0) {
        return {StorageError::invalid_data, std::nullopt, "invalid account data"};
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (usernames_.count(username) > 0) {
        return {StorageError::duplicate, std::nullopt, "username already exists"};
    }
    UserRecord record;
    record.id = next_user_id_++;
    record.username = std::move(username);
    record.password_hash = std::move(password_hash);
    record.wallet_balance = initial_chips;
    usernames_.emplace(record.username, record.id);
    users_.emplace(record.id, record);
    return {StorageError::ok, record, {}};
}

StorageResult<UserRecord> InMemoryAccountStore::findUserByName(std::string_view username) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = usernames_.find(std::string(username));
    if (name == usernames_.end()) {
        return {StorageError::not_found, std::nullopt, "user not found"};
    }
    return {StorageError::ok, users_.at(name->second), {}};
}

StorageResult<UserRecord> InMemoryAccountStore::findUserById(UserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto user = users_.find(user_id);
    if (user == users_.end()) {
        return {StorageError::not_found, std::nullopt, "user not found"};
    }
    return {StorageError::ok, user->second, {}};
}

StorageError InMemoryAccountStore::storeSession(SessionRecord session) {
    if (session.user_id == 0 || session.token_hash.empty() || session.expires_at_unix_ms <= 0) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_.count(session.user_id) == 0) {
        return StorageError::not_found;
    }
    const auto token_hash = session.token_hash;
    if (!sessions_.emplace(token_hash, std::move(session)).second) {
        return StorageError::conflict;
    }
    return StorageError::ok;
}

StorageResult<SessionRecord> InMemoryAccountStore::findSession(std::string_view token_hash,
                                                                std::int64_t now_unix_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = sessions_.find(std::string(token_hash));
    if (session == sessions_.end() || session->second.revoked
        || session->second.expires_at_unix_ms <= now_unix_ms) {
        return {StorageError::not_found, std::nullopt, "session not found or expired"};
    }
    return {StorageError::ok, session->second, {}};
}

StorageError InMemoryAccountStore::rotateSession(std::string_view current_token_hash,
                                                  SessionRecord replacement,
                                                  std::int64_t now_unix_ms) {
    if (replacement.user_id == 0 || replacement.token_hash.empty()
        || replacement.expires_at_unix_ms <= now_unix_ms) {
        return StorageError::invalid_data;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current_key = std::string(current_token_hash);
    const auto current = sessions_.find(current_key);
    if (current == sessions_.end() || current->second.revoked
        || current->second.expires_at_unix_ms <= now_unix_ms
        || current->second.user_id != replacement.user_id) {
        return StorageError::not_found;
    }
    if (replacement.token_hash != current_key
        && sessions_.count(replacement.token_hash) != 0) {
        return StorageError::conflict;
    }
    const auto replacement_key = replacement.token_hash;
    sessions_[replacement_key] = std::move(replacement);
    if (replacement_key != current_key) {
        sessions_.erase(current_key);
    }
    return StorageError::ok;
}

StorageError InMemoryAccountStore::revokeSession(std::string_view token_hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = sessions_.find(std::string(token_hash));
    if (session == sessions_.end()) {
        return StorageError::not_found;
    }
    session->second.revoked = true;
    return StorageError::ok;
}

StorageError InMemoryAccountStore::updateSessionSequence(std::string_view token_hash,
                                                          std::uint64_t expected,
                                                          std::uint64_t replacement) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto session = sessions_.find(std::string(token_hash));
    if (session == sessions_.end() || session->second.revoked) {
        return StorageError::not_found;
    }
    if (session->second.last_client_sequence != expected || replacement != expected + 1) {
        return StorageError::conflict;
    }
    session->second.last_client_sequence = replacement;
    return StorageError::ok;
}

}  // namespace poker::storage
