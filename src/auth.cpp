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

BCRYPT_ALG_HANDLE ecdhAlgorithm() {
    static BCRYPT_ALG_HANDLE alg = openAlgorithm(BCRYPT_ECDH_P256_ALGORITHM, 0);
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

// Generates a P-256 key pair of `alg` and exports its public point; nullptr when crypto fails.
BCRYPT_KEY_HANDLE generateKeyPair(BCRYPT_ALG_HANDLE alg, PublicKey& out) {
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!alg || BCryptGenerateKeyPair(alg, &key, 256, 0) != 0) return nullptr;
    PublicBlob blob{};
    ULONG size = 0;
    if (BCryptFinalizeKeyPair(key, 0) != 0 ||
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, reinterpret_cast<PUCHAR>(&blob), sizeof(blob), &size, 0) != 0 ||
        size != sizeof(blob) || blob.header.cbKey != 32) {
        BCryptDestroyKey(key);
        return nullptr;
    }
    memcpy(out.data(), blob.xy, out.size());
    return key;
}

// Imports a P-256 public point for `alg`; nullptr for a point that is not on the curve.
BCRYPT_KEY_HANDLE importPublic(BCRYPT_ALG_HANDLE alg, ULONG magic, const PublicKey& key) {
    PublicBlob blob{};
    blob.header.dwMagic = magic;
    blob.header.cbKey = 32;
    memcpy(blob.xy, key.data(), key.size());
    BCRYPT_KEY_HANDLE pub = nullptr;
    if (!alg || BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &pub, reinterpret_cast<PUCHAR>(&blob),
                                    sizeof(blob), 0) != 0)
        return nullptr;
    return pub;
}

}  // namespace

std::optional<Digest> hmacSha256(const std::string& key, const uint8_t* data, size_t size) {
    Digest digest{};
    if (!digestOf(hmacAlgorithm(), &key, data, size, digest)) return std::nullopt;
    return digest;
}

std::optional<std::array<uint8_t, 8>> hmacTag(const std::string& key, const uint8_t* data, size_t size) {
    auto digest = hmacSha256(key, data, size);
    if (!digest) return std::nullopt;
    std::array<uint8_t, 8> tag{};
    memcpy(tag.data(), digest->data(), tag.size());
    return tag;
}

LinkMac::LinkMac(const LinkKey& key) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    BCRYPT_ALG_HANDLE alg = hmacAlgorithm();
    if (alg && BCryptCreateHash(alg, &hash, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(),
                                BCRYPT_HASH_REUSABLE_FLAG) == 0)
        hash_ = hash;
}

LinkMac::~LinkMac() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
}

LinkMac& LinkMac::operator=(LinkMac&& other) noexcept {
    if (this != &other) {
        if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
        hash_ = other.hash_;
        other.hash_ = nullptr;
    }
    return *this;
}

bool LinkMac::tag(const uint8_t* data, size_t size, uint8_t* out) {
    Digest digest{};
    auto hash = static_cast<BCRYPT_HASH_HANDLE>(hash_);
    // A reusable hash object starts over, keyed, after every BCryptFinishHash.
    if (!hash || BCryptHashData(hash, (PUCHAR)data, (ULONG)size, 0) != 0 ||
        BCryptFinishHash(hash, digest.data(), (ULONG)digest.size(), 0) != 0)
        return false;
    memcpy(out, digest.data(), kLinkTagBytes);
    return true;
}

bool LinkMac::check(const uint8_t* data, size_t size, const uint8_t* expected) {
    uint8_t mine[kLinkTagBytes];
    if (!tag(data, size, mine)) return false;  // no tag computed: reject
    uint8_t diff = 0;
    for (size_t i = 0; i < kLinkTagBytes; ++i) diff |= mine[i] ^ expected[i];  // constant time
    return diff == 0;
}

std::unique_ptr<EcdhKey> EcdhKey::generate() {
    std::unique_ptr<EcdhKey> k(new EcdhKey());
    k->key_ = generateKeyPair(ecdhAlgorithm(), k->public_);
    return k->key_ ? std::move(k) : nullptr;
}

EcdhKey::~EcdhKey() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

std::optional<Digest> EcdhKey::agree(const PublicKey& peer) const {
    BCRYPT_KEY_HANDLE pub = importPublic(ecdhAlgorithm(), BCRYPT_ECDH_PUBLIC_P256_MAGIC, peer);
    if (!pub) return std::nullopt;
    BCRYPT_SECRET_HANDLE secret = nullptr;
    NTSTATUS agreed = BCryptSecretAgreement(static_cast<BCRYPT_KEY_HANDLE>(key_), pub, &secret, 0);
    BCryptDestroyKey(pub);
    if (agreed != 0) return std::nullopt;
    BCryptBuffer param{static_cast<ULONG>(sizeof(BCRYPT_SHA256_ALGORITHM)), KDF_HASH_ALGORITHM,
                       const_cast<wchar_t*>(BCRYPT_SHA256_ALGORITHM)};
    BCryptBufferDesc params{BCRYPTBUFFER_VERSION, 1, &param};
    Digest out{};
    ULONG size = 0;
    bool ok = BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &params, out.data(), (ULONG)out.size(), &size, 0) == 0 &&
              size == out.size();
    BCryptDestroySecret(secret);
    if (!ok) return std::nullopt;
    return out;
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
    std::shared_ptr<Identity> id(new Identity());
    id->key_ = generateKeyPair(ecdsaAlgorithm(), id->public_);  // destroyed with `id`
    if (!id->key_) return nullptr;
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
    BCRYPT_KEY_HANDLE pub = importPublic(ecdsaAlgorithm(), BCRYPT_ECDSA_PUBLIC_P256_MAGIC, key);
    if (!pub) return false;
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
