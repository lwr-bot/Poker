#include "poker/security/sodium_crypto_provider.hpp"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace poker::security {

SodiumCryptoProvider::SodiumCryptoProvider() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

std::string SodiumCryptoProvider::hashPassword(std::string_view password) {
    if (password.empty() || password.size() > crypto_pwhash_PASSWD_MAX) {
        throw std::invalid_argument("password length is outside libsodium limits");
    }
    std::array<char, crypto_pwhash_STRBYTES> encoded{};
    if (crypto_pwhash_str_alg(encoded.data(),
                              password.data(),
                              static_cast<unsigned long long>(password.size()),
                              crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE,
                              crypto_pwhash_ALG_ARGON2ID13)
        != 0) {
        throw std::runtime_error("Argon2id password hashing failed");
    }
    return encoded.data();
}

bool SodiumCryptoProvider::verifyPassword(std::string_view password,
                                          std::string_view encoded_hash) {
    if (encoded_hash.empty() || encoded_hash.size() >= crypto_pwhash_STRBYTES) {
        return false;
    }
    const std::string terminated_hash(encoded_hash);
    return crypto_pwhash_str_verify(terminated_hash.c_str(),
                                    password.data(),
                                    static_cast<unsigned long long>(password.size()))
           == 0;
}

std::string SodiumCryptoProvider::generateSessionToken() {
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, bytes.size() * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}

std::string SodiumCryptoProvider::hashSessionToken(std::string_view raw_token) {
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),
                       reinterpret_cast<const unsigned char*>(raw_token.data()),
                       static_cast<unsigned long long>(raw_token.size()));
    std::array<char, digest.size() * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), digest.data(), digest.size());
    return encoded.data();
}

}  // namespace poker::security

