#include "auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstring>
#include <mutex>

namespace dn {
namespace {

constexpr size_t kCommitmentBytes = 16;

BCRYPT_ALG_HANDLE openAlgorithm(const wchar_t* name, ULONG flags) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    return BCryptOpenAlgorithmProvider(&alg, name, nullptr, flags) == 0 ? alg : nullptr;
}

BCRYPT_ALG_HANDLE hmacAlgorithm() {
    static BCRYPT_ALG_HANDLE alg = openAlgorithm(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return alg;
}

BCRYPT_ALG_HANDLE hashAlgorithm() {
    static BCRYPT_ALG_HANDLE alg = openAlgorithm(BCRYPT_SHA256_ALGORITHM, 0);
    return alg;
}

BCRYPT_ALG_HANDLE ecdsaAlgorithm() {
    static BCRYPT_ALG_HANDLE alg = openAlgorithm(BCRYPT_ECDSA_P256_ALGORITHM, 0);
    return alg;
}

// SHA-256, keyed (HMAC) when `key` is given.
bool digestOf(BCRYPT_ALG_HANDLE alg, const std::string* key, const uint8_t* data, size_t size, Digest& out) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    PUCHAR secret = key ? (PUCHAR)key->data() : nullptr;
    ULONG secretSize = key ? (ULONG)key->size() : 0;
    if (!alg || BCryptCreateHash(alg, &hash, nullptr, 0, secret, secretSize, 0) != 0) return false;
    bool ok = BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) == 0 &&
              BCryptFinishHash(hash, out.data(), (ULONG)out.size(), 0) == 0;
    BCryptDestroyHash(hash);
    return ok;
}

// BCRYPT_ECCPUBLIC_BLOB: header, then X and Y.
struct PublicBlob {
    BCRYPT_ECCKEY_BLOB header;
    uint8_t xy[64];
};

}  // namespace

std::optional<std::array<uint8_t, 8>> hmacTag(const std::string& key, const uint8_t* data, size_t size) {
    Digest digest{};
    if (!digestOf(hmacAlgorithm(), &key, data, size, digest)) return std::nullopt;
    std::array<uint8_t, 8> tag{};
    memcpy(tag.data(), digest.data(), tag.size());
    return tag;
}

std::optional<Digest> sha256(const uint8_t* data, size_t size) {
    Digest digest{};
    if (!digestOf(hashAlgorithm(), nullptr, data, size, digest)) return std::nullopt;
    return digest;
}

bool randomBytes(uint8_t* out, size_t size) {
    return BCryptGenRandom(nullptr, out, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::shared_ptr<const Identity> Identity::generate() {
    BCRYPT_ALG_HANDLE alg = ecdsaAlgorithm();
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!alg || BCryptGenerateKeyPair(alg, &key, 256, 0) != 0) return nullptr;
    std::shared_ptr<Identity> id(new Identity());
    id->key_ = key;  // destroyed with `id` from here on
    PublicBlob blob{};
    ULONG size = 0;
    if (BCryptFinalizeKeyPair(key, 0) != 0 ||
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, reinterpret_cast<PUCHAR>(&blob), sizeof(blob), &size, 0) != 0 ||
        size != sizeof(blob) || blob.header.cbKey != 32)
        return nullptr;
    memcpy(id->public_.data(), blob.xy, id->public_.size());
    id->commitment_ = identityCommitment(id->public_);
    if (id->commitment_.empty()) return nullptr;
    return id;
}

Identity::~Identity() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

std::optional<Signature> Identity::sign(const Digest& digest) const {
    std::lock_guard<std::mutex> lock(mu_);
    Signature sig{};
    ULONG size = 0;
    if (BCryptSignHash(static_cast<BCRYPT_KEY_HANDLE>(key_), nullptr, (PUCHAR)digest.data(), (ULONG)digest.size(),
                       sig.data(), (ULONG)sig.size(), &size, 0) != 0 ||
        size != sig.size())
        return std::nullopt;
    return sig;
}

std::string identityCommitment(const PublicKey& key) {
    auto digest = sha256(key.data(), key.size());
    if (!digest) return {};
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < kCommitmentBytes; ++i) {
        s += hex[(*digest)[i] >> 4];
        s += hex[(*digest)[i] & 15];
    }
    return s;
}

bool verifySignature(const PublicKey& key, const Digest& digest, const Signature& signature) {
    BCRYPT_ALG_HANDLE alg = ecdsaAlgorithm();
    PublicBlob blob{};
    blob.header.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    blob.header.cbKey = 32;
    memcpy(blob.xy, key.data(), key.size());
    BCRYPT_KEY_HANDLE pub = nullptr;
    // Import fails for a point that is not on the curve.
    if (!alg || BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &pub, reinterpret_cast<PUCHAR>(&blob),
                                    sizeof(blob), 0) != 0)
        return false;
    bool ok = BCryptVerifySignature(pub, nullptr, (PUCHAR)digest.data(), (ULONG)digest.size(), (PUCHAR)signature.data(),
                                    (ULONG)signature.size(), 0) == 0;
    BCryptDestroyKey(pub);
    return ok;
}

std::shared_ptr<const Identity> processIdentity() {
    static const std::shared_ptr<const Identity> id = Identity::generate();
    return id;
}

}  // namespace dn
