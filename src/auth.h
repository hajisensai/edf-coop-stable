#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace dn {

// HMAC-SHA256(key, data) truncated to 8 bytes.
std::array<uint8_t, 8> hmacTag(const std::string& key, const uint8_t* data, size_t size);

}  // namespace dn
