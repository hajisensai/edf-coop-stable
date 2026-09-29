#include "auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <mutex>

namespace dn {
namespace {

BCRYPT_ALG_HANDLE hmacAlgorithm() {
    static BCRYPT_ALG_HANDLE alg = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    });
    return alg;
}

}  // namespace

std::optional<std::array<uint8_t, 8>> hmacTag(const std::string& key, const uint8_t* data, size_t size) {
    uint8_t digest[32] = {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCRYPT_ALG_HANDLE alg = hmacAlgorithm();
    if (!alg || BCryptCreateHash(alg, &hash, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0)
        return std::nullopt;
    bool ok = BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) == 0 &&
              BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    BCryptDestroyHash(hash);
    if (!ok) return std::nullopt;
    std::array<uint8_t, 8> tag{};
    memcpy(tag.data(), digest, tag.size());
    return tag;
}

}  // namespace dn
