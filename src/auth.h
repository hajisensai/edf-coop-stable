#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace dn {

// HMAC-SHA256(key, data) truncated to 8 bytes; nullopt when the OS crypto provider fails. Callers
// must then reject: a fixed fallback tag would make every forged packet verify.
std::optional<std::array<uint8_t, 8>> hmacTag(const std::string& key, const uint8_t* data, size_t size);

using Digest = std::array<uint8_t, 32>;
using PublicKey = std::array<uint8_t, 64>;  // ECDSA P-256 point, X then Y
using Signature = std::array<uint8_t, 64>;  // r then s

// SHA-256; nullopt when the OS crypto provider fails.
std::optional<Digest> sha256(const uint8_t* data, size_t size);
// HMAC-SHA256, all 32 bytes; nullopt when the OS crypto provider fails.
std::optional<Digest> hmacSha256(const std::string& key, const uint8_t* data, size_t size);
// Cryptographically random bytes; false when the OS provider fails.
bool randomBytes(uint8_t* out, size_t size);

// A player's direct-link identity: an ECDSA P-256 key pair made for this game session. Its
// commitment goes into the player's own lobby member attributes, which only that EOS user can write,
// so a hello signed with the key proves the EOS id it claims (see DirectNet's hello handling).
class Identity {
public:
    // nullptr when the OS crypto provider fails.
    static std::shared_ptr<const Identity> generate();
    ~Identity();
    Identity(const Identity&) = delete;
    Identity& operator=(const Identity&) = delete;

    const PublicKey& publicKey() const { return public_; }
    // What the player publishes: identityCommitment(publicKey()).
    const std::string& commitment() const { return commitment_; }
    std::optional<Signature> sign(const Digest& digest) const;
    // Numbers this identity's direct-link sessions; strictly increasing, so a host can tell a newer
    // session from a replayed older one.
    uint64_t nextSession() const { return ++session_; }

private:
    Identity() = default;

    void* key_ = nullptr;  // BCRYPT_KEY_HANDLE
    PublicKey public_{};
    std::string commitment_;
    mutable std::mutex mu_;  // one signature at a time per key handle
    mutable std::atomic<uint64_t> session_{0};
};

using LinkKey = Digest;
constexpr size_t kLinkTagBytes = 16;

// HMAC-SHA256 with one link direction's key, truncated to kLinkTagBytes. Keeps one reusable hash
// object (BCRYPT_HASH_REUSABLE_FLAG) instead of setting up the key for every packet. Not thread-safe.
class LinkMac {
public:
    LinkMac() = default;
    explicit LinkMac(const LinkKey& key);
    ~LinkMac();
    LinkMac(LinkMac&& other) noexcept : hash_(other.hash_) { other.hash_ = nullptr; }
    LinkMac& operator=(LinkMac&& other) noexcept;
    LinkMac(const LinkMac&) = delete;
    LinkMac& operator=(const LinkMac&) = delete;

    // False when the OS crypto provider failed (or default-constructed): nothing can be tagged.
    bool valid() const { return hash_ != nullptr; }
    // Writes kLinkTagBytes to `out`; false when crypto fails.
    bool tag(const uint8_t* data, size_t size, uint8_t* out);
    // Constant-time check of the kLinkTagBytes at `expected`.
    bool check(const uint8_t* data, size_t size, const uint8_t* expected);

private:
    void* hash_ = nullptr;  // BCRYPT_HASH_HANDLE
};

// An ephemeral ECDH P-256 key pair: one per direct-link session, never reused, never stored.
class EcdhKey {
public:
    // nullptr when the OS crypto provider fails.
    static std::unique_ptr<EcdhKey> generate();
    ~EcdhKey();
    EcdhKey(const EcdhKey&) = delete;
    EcdhKey& operator=(const EcdhKey&) = delete;

    const PublicKey& publicKey() const { return public_; }
    // SHA-256 of the secret shared with `peer`; nullopt for a point not on the curve or a crypto failure.
    std::optional<Digest> agree(const PublicKey& peer) const;

private:
    EcdhKey() = default;

    void* key_ = nullptr;  // BCRYPT_KEY_HANDLE
    PublicKey public_{};
};

// 32 lowercase hex digits of SHA-256(public key); "" when the crypto provider fails.
std::string identityCommitment(const PublicKey& key);
bool verifySignature(const PublicKey& key, const Digest& digest, const Signature& signature);

// The identity this process publishes and signs its hellos with; created on first use, nullptr when
// the crypto provider fails.
std::shared_ptr<const Identity> processIdentity();

}  // namespace dn
