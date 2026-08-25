#ifndef POKER_STORAGE_IN_MEMORY_ACCOUNT_STORE_HPP
#define POKER_STORAGE_IN_MEMORY_ACCOUNT_STORE_HPP

#include "poker/storage/account_store.hpp"

#include <mutex>
#include <string>
#include <unordered_map>

namespace poker::storage {

class InMemoryAccountStore final : public AccountStore {
public:
    StorageResult<UserRecord> createUser(std::string username,
                                          std::string password_hash,
                                          std::int64_t initial_chips) override;
    StorageResult<UserRecord> findUserByName(std::string_view username) override;
    StorageResult<UserRecord> findUserById(UserId user_id) override;
    StorageError storeSession(SessionRecord session) override;
    StorageResult<SessionRecord> findSession(std::string_view token_hash,
                                              std::int64_t now_unix_ms) override;
    StorageError rotateSession(std::string_view current_token_hash,
                               SessionRecord replacement,
                               std::int64_t now_unix_ms) override;
    StorageError revokeSession(std::string_view token_hash) override;
    StorageError updateSessionSequence(std::string_view token_hash,
                                        std::uint64_t expected,
                                        std::uint64_t replacement) override;

private:
    std::mutex mutex_;
    UserId next_user_id_{1};
    std::unordered_map<UserId, UserRecord> users_;
    std::unordered_map<std::string, UserId> usernames_;
    std::unordered_map<std::string, SessionRecord> sessions_;
};

}  // namespace poker::storage

#endif
