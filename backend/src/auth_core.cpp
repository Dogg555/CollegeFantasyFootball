#include "auth_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <crypt.h>
#include <fstream>
#include <random>
#include <string_view>
#include <utility>

namespace cff::auth {
namespace {

template <std::size_t N>
bool fillFromUrandom(std::array<unsigned char, N> &bytes) {
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    if (!urandom.is_open()) {
        return false;
    }
    urandom.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return urandom.gcount() == static_cast<std::streamsize>(bytes.size());
}

} // namespace

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool credentialsValid(const std::string &email,
                      const std::string &password,
                      std::size_t minimumPasswordLength,
                      std::size_t maximumPasswordLength) {
    constexpr std::size_t kMaximumEmailLength = 254;
    if (email.empty() || email.size() > kMaximumEmailLength) {
        return false;
    }
    return password.size() >= minimumPasswordLength
        && password.size() <= maximumPasswordLength;
}

std::optional<std::string> hashPassword(const std::string &password) {
    constexpr int kCost = 12;
    constexpr std::size_t kSaltLength = 16;
    std::array<unsigned char, kSaltLength> saltBytes{};
    if (!fillFromUrandom(saltBytes)) {
        std::random_device randomDevice;
        for (auto &byte : saltBytes) {
            byte = static_cast<unsigned char>(randomDevice());
        }
    }

    char saltBuffer[128];
    if (!crypt_gensalt_rn("$2b$", kCost,
                          reinterpret_cast<const char *>(saltBytes.data()),
                          saltBytes.size(),
                          saltBuffer, sizeof(saltBuffer))) {
        return std::nullopt;
    }

    struct crypt_data data;
    data.initialized = 0;
    const char *hash = crypt_r(password.c_str(), saltBuffer, &data);
    if (!hash) {
        return std::nullopt;
    }
    return std::string{hash};
}

bool verifyPassword(const std::string &password, const std::string &hash) {
    struct crypt_data data;
    data.initialized = 0;
    const char *computed = crypt_r(password.c_str(), hash.c_str(), &data);
    if (!computed) {
        return false;
    }
    return hash == computed;
}

std::optional<std::string> bearerTokenFromHeader(const std::string &authorizationHeader) {
    if (authorizationHeader.size() < 8) {
        return std::nullopt;
    }
    constexpr std::string_view kBearerPrefix = "Bearer ";
    if (authorizationHeader.rfind(kBearerPrefix, 0) != 0) {
        return std::nullopt;
    }
    return authorizationHeader.substr(kBearerPrefix.size());
}

std::string randomToken() {
    constexpr std::size_t kTokenBytes = 32;
    std::array<unsigned char, kTokenBytes> bytes{};
    if (!fillFromUrandom(bytes)) {
        std::random_device randomDevice;
        for (auto &byte : bytes) {
            byte = static_cast<unsigned char>(randomDevice());
        }
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(6 + bytes.size() * 2);
    token.append("token-");
    for (const auto byte : bytes) {
        token.push_back(kHex[byte >> 4]);
        token.push_back(kHex[byte & 0x0F]);
    }
    return token;
}

} // namespace cff::auth
