#pragma once
#include <cstdint>

namespace dn::extension {
// Shared by actual admission and the optional capacity query. These are ceilings,
// not reserved memory: ordinary game reliable datagrams share each link's backlog.
inline constexpr uint32_t kInboxMessages = 4096;
inline constexpr uint32_t kPendingBytes = 1024 * 1024;
inline constexpr uint32_t kCongestedBacklogBytes = kPendingBytes;
} // namespace dn::extension
