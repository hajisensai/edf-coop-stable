#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace dn {

// HMAC-SHA256(key, data) truncated to 8 bytes; nullopt when the OS crypto provider fails. Callers
// must then reject: a fixed fallback tag would make every forged packet verify.
std::optional<std::array<uint8_t, 8>> hmacTag(const std::string& key, const uint8_t* data, size_t size);

}  // namespace dn
