#include "poker/security/auth_service.hpp"

#include <cctype>
#include <limits>
#include <utility>

namespace poker::security {
AuthService::AuthService(storage::AccountStore& store,
                         CryptoProvider& crypto,
                         std::uint32_t session_ttl_seconds,
                         std::int64_t initial_chips)
    : store_(store),
      crypto_(crypto),
      session_ttl_seconds_(session_ttl_seconds),
      initial_chips_(initial_chips),
      dummy_password_hash_(crypto.hashPassword("not-a-real-user-password")) {}

AuthResult<storage::UserRecord> AuthService::registerUser(std::string username,
                                                          std::string_view password) {
    if (!validUsername(username)) {
        return {AuthError::invalid_username,
                std::nullopt,
                "username must be 3-32 ASCII letters, digits, or underscores"};
    }
    if (!validPassword(password)) {
        return {AuthError::weak_password,
                std::nullopt,
                "password must contain 10-128 characters"};
    }

    auto created = store_.createUser(std::move(username),
                                     crypto_.hashPassword(password),
                                     initial_chips_);
    if (!created) {
        if (created.error == storage::StorageError::duplicate) {
            return {AuthError::username_taken, std::nullopt, "username already exists"};
        }
        return {AuthError::storage_unavailable, std::nullopt, created.message};
    }
    return {AuthError::ok, *created.value, {}};
}

AuthResult<AuthenticatedSession> AuthService::login(std::string_view username,
                                                     std::string_view password,
                                                     std::int64_t now_unix_ms) {
    const auto user = store_.findUserByName(username);
    const auto& encoded_hash = user ? user.value->password_hash : dummy_password_hash_;
    const bool password_matches = crypto_.verifyPassword(password, encoded_hash);
    if (!user || !password_matches) {
        return {user.error == storage::StorageError::unavailable
                    ? AuthError::storage_unavailable
                    : AuthError::invalid_credentials,
                std::nullopt,
                "invalid username or password"};
    }
    if (user.value->disabled) {
        return {AuthError::account_disabled, std::nullopt, "account is disabled"};
    }

    const auto raw_token = crypto_.generateSessionToken();
    const auto token_hash = crypto_.hashSessionToken(raw_token);
    const auto expires = now_unix_ms + static_cast<std::int64_t>(session_ttl_seconds_) * 1000;
    storage::SessionRecord session;
    session.user_id = user.value->id;
    session.token_hash = token_hash;
    session.expires_at_unix_ms = expires;
    const auto stored = store_.storeSession(std::move(session));
    if (stored != storage::StorageError::ok) {
        return {AuthError::storage_unavailable, std::nullopt, "could not create session"};
    }

    return {AuthError::ok,
            AuthenticatedSession{user.value->id,
                                 user.value->username,
                                 raw_token,
                                 user.value->wallet_balance,
                                 expires,
                                 0},
            {}};
}

AuthResult<AuthenticatedSession> AuthService::authenticate(std::string_view raw_token,
                                                            std::int64_t now_unix_ms) {
    if (raw_token.empty()) {
        return {AuthError::invalid_session, std::nullopt, "session token is required"};
    }
    const auto token_hash = crypto_.hashSessionToken(raw_token);
    const auto session = store_.findSession(token_hash, now_unix_ms);
    if (!session) {
        return {session.error == storage::StorageError::unavailable
                    ? AuthError::storage_unavailable
                    : AuthError::invalid_session,
                std::nullopt,
                session.message};
    }
    const auto user = store_.findUserById(session.value->user_id);
    if (!user || user.value->disabled) {
        return {AuthError::invalid_session, std::nullopt, "session user is unavailable"};
    }
    return {AuthError::ok,
            AuthenticatedSession{user.value->id,
                                 user.value->username,
                                 {},
                                 user.value->wallet_balance,
                                 session.value->expires_at_unix_ms,
                                 session.value->last_client_sequence},
            {}};
}

AuthResult<AuthenticatedSession> AuthService::refresh(std::string_view raw_token,
                                                       std::int64_t now_unix_ms) {
    const auto authenticated = authenticate(raw_token, now_unix_ms);
    if (!authenticated) {
        return authenticated;
    }

    const auto new_raw_token = crypto_.generateSessionToken();
    const auto expires = now_unix_ms + static_cast<std::int64_t>(session_ttl_seconds_) * 1000;
    storage::SessionRecord replacement;
    replacement.user_id = authenticated.value->user_id;
    replacement.token_hash = crypto_.hashSessionToken(new_raw_token);
    replacement.expires_at_unix_ms = expires;
    replacement.last_client_sequence = authenticated.value->last_client_sequence;
    const auto rotated = store_.rotateSession(crypto_.hashSessionToken(raw_token),
                                              std::move(replacement),
                                              now_unix_ms);
    if (rotated != storage::StorageError::ok) {
        return {rotated == storage::StorageError::unavailable
                    ? AuthError::storage_unavailable
                    : AuthError::invalid_session,
                std::nullopt,
                "session could not be refreshed"};
    }

    auto result = *authenticated.value;
    result.raw_token = new_raw_token;
    result.expires_at_unix_ms = expires;
    return {AuthError::ok, std::move(result), {}};
}

AuthError AuthService::logout(std::string_view raw_token) {
    if (raw_token.empty()) {
        return AuthError::invalid_session;
    }
    const auto error = store_.revokeSession(crypto_.hashSessionToken(raw_token));
    if (error == storage::StorageError::ok) {
        return AuthError::ok;
    }
    return error == storage::StorageError::unavailable ? AuthError::storage_unavailable
                                                       : AuthError::invalid_session;
}

bool AuthService::validUsername(std::string_view username) noexcept {
    if (username.size() < 3 || username.size() > 32) {
        return false;
    }
    for (const auto character : username) {
        const auto value = static_cast<unsigned char>(character);
        if (std::isalnum(value) == 0 && character != '_') {
            return false;
        }
    }
    return true;
}

bool AuthService::validPassword(std::string_view password) noexcept {
    return password.size() >= 10 && password.size() <= 128;
}

std::string toString(AuthError error) {
    switch (error) {
    case AuthError::ok: return "ok";
    case AuthError::invalid_username: return "invalid username";
    case AuthError::weak_password: return "weak password";
    case AuthError::username_taken: return "username taken";
    case AuthError::invalid_credentials: return "invalid credentials";
    case AuthError::account_disabled: return "account disabled";
    case AuthError::invalid_session: return "invalid session";
    case AuthError::storage_unavailable: return "storage unavailable";
    }
    return "unknown auth error";
}

}  // namespace poker::security
