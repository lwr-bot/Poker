#ifndef POKER_SECURITY_SODIUM_CRYPTO_PROVIDER_HPP
#define POKER_SECURITY_SODIUM_CRYPTO_PROVIDER_HPP

#include "poker/security/crypto_provider.hpp"

namespace poker::security {

class SodiumCryptoProvider final : public CryptoProvider {
public:
    SodiumCryptoProvider();

    std::string hashPassword(std::string_view password) override;
    bool verifyPassword(std::string_view password, std::string_view encoded_hash) override;
    std::string generateSessionToken() override;
    std::string hashSessionToken(std::string_view raw_token) override;
};

}  // namespace poker::security

#endif

