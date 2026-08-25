#ifndef POKER_SECURITY_CRYPTO_PROVIDER_HPP
#define POKER_SECURITY_CRYPTO_PROVIDER_HPP

#include <string>
#include <string_view>

namespace poker::security {

class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;

    virtual std::string hashPassword(std::string_view password) = 0;
    virtual bool verifyPassword(std::string_view password, std::string_view encoded_hash) = 0;
    virtual std::string generateSessionToken() = 0;
    virtual std::string hashSessionToken(std::string_view raw_token) = 0;
};

}  // namespace poker::security

#endif

