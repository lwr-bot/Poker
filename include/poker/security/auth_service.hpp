#ifndef POKER_SECURITY_AUTH_SERVICE_HPP
#define POKER_SECURITY_AUTH_SERVICE_HPP

#include "poker/security/crypto_provider.hpp"
#include "poker/storage/account_store.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace poker::security {

enum class AuthError : std::uint8_t {
    ok = 0,
    invalid_username,
    weak_password,
    username_taken,
    invalid_credentials,
    account_disabled,
    invalid_session,
    storage_unavailable,
};

struct AuthenticatedSession {
    storage::UserId user_id{0};
    std::string username;
    std::string raw_token;
    std::int64_t wallet_balance{0};
    std::int64_t expires_at_unix_ms{0};
    std::uint64_t last_client_sequence{0};
};

template <typename T>
struct AuthResult {
    AuthError error{AuthError::ok};
    std::optional<T> value;
    std::string message;

    explicit operator bool() const noexcept {
        return error == AuthError::ok && value.has_value();
    }
};

class AuthService {
public:
    AuthService(storage::AccountStore& store,
                CryptoProvider& crypto,
                std::uint32_t session_ttl_seconds = 86'400,
                std::int64_t initial_chips = 100'000);

    AuthResult<storage::UserRecord> registerUser(std::string username,
                                                 std::string_view password);
    AuthResult<AuthenticatedSession> login(std::string_view username,
                                            std::string_view password,
                                            std::int64_t now_unix_ms);
    AuthResult<AuthenticatedSession> authenticate(std::string_view raw_token,
                                                   std::int64_t now_unix_ms);
    AuthResult<AuthenticatedSession> refresh(std::string_view raw_token,
                                             std::int64_t now_unix_ms);
    AuthError logout(std::string_view raw_token);

    static bool validUsername(std::string_view username) noexcept;
    static bool validPassword(std::string_view password) noexcept;

private:
    storage::AccountStore& store_;
    CryptoProvider& crypto_;
    std::uint32_t session_ttl_seconds_;
    std::int64_t initial_chips_;
    std::string dummy_password_hash_;
};

std::string toString(AuthError error);

}  // namespace poker::security

#endif
