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

// 32 lowercase hex digits of SHA-256(public key); "" when the crypto provider fails.
std::string identityCommitment(const PublicKey& key);
bool verifySignature(const PublicKey& key, const Digest& digest, const Signature& signature);

// The identity this process publishes and signs its hellos with; created on first use, nullptr when
// the crypto provider fails.
std::shared_ptr<const Identity> processIdentity();

}  // namespace dn
