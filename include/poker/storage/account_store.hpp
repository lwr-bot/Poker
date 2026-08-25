#ifndef POKER_STORAGE_ACCOUNT_STORE_HPP
#define POKER_STORAGE_ACCOUNT_STORE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace poker::storage {

using UserId = std::uint64_t;

enum class StorageError : std::uint8_t {
    ok = 0,
    duplicate,
    not_found,
    insufficient_funds,
    conflict,
    unavailable,
    invalid_data,
};

template <typename T>
struct StorageResult {
    StorageError error{StorageError::ok};
    std::optional<T> value;
    std::string message;

    explicit operator bool() const noexcept {
        return error == StorageError::ok && value.has_value();
    }
};

struct UserRecord {
    UserId id{0};
    std::string username;
    std::string password_hash;
    std::int64_t wallet_balance{0};
    bool disabled{false};
};

struct SessionRecord {
    UserId user_id{0};
    std::string token_hash;
    std::int64_t expires_at_unix_ms{0};
    std::uint64_t last_client_sequence{0};
    bool revoked{false};
};

class AccountStore {
public:
    virtual ~AccountStore() = default;

    virtual StorageResult<UserRecord> createUser(std::string username,
                                                  std::string password_hash,
                                                  std::int64_t initial_chips) = 0;
    virtual StorageResult<UserRecord> findUserByName(std::string_view username) = 0;
    virtual StorageResult<UserRecord> findUserById(UserId user_id) = 0;
    virtual StorageError storeSession(SessionRecord session) = 0;
    virtual StorageResult<SessionRecord> findSession(std::string_view token_hash,
                                                      std::int64_t now_unix_ms) = 0;
    virtual StorageError rotateSession(std::string_view current_token_hash,
                                       SessionRecord replacement,
                                       std::int64_t now_unix_ms) = 0;
    virtual StorageError revokeSession(std::string_view token_hash) = 0;
    virtual StorageError updateSessionSequence(std::string_view token_hash,
                                                std::uint64_t expected,
                                                std::uint64_t replacement) = 0;
};

}  // namespace poker::storage

#endif
