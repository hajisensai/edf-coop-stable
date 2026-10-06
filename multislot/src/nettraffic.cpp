#include "nettraffic.h"

#include <cstddef>
#include <cstring>

#include "src/netclass.h"

namespace multislot {
namespace {

constexpr std::uint32_t kFlush = 0x12CEA10;
// The per-peer datagram buffer the flush takes (rcx): a std::vector<uint8_t> at +0x20 (begin) / +0x28 (end), the
// 8-byte header and the records (12CEA40..12CEA51).
constexpr std::size_t kBufferBegin = 0x20, kBufferEnd = 0x28;
// The game builds datagrams of at most 1100 bytes plus one record of at most 1400 (12CFFD0, 12D0BE6): anything far
// bigger is not a datagram this hook understands.
constexpr std::size_t kLargestDatagram = 0x1000;

using FlushFn = void(__fastcall*)(void* buffer);
FlushFn flush = nullptr;

void __fastcall NetFlushHook(void* buffer) {
    const auto* object = static_cast<const std::uint8_t*>(buffer);
    const std::uint8_t* begin = nullptr;
    const std::uint8_t* end = nullptr;
    std::memcpy(&begin, object + kBufferBegin, sizeof(begin));
    std::memcpy(&end, object + kBufferEnd, sizeof(end));
    if (begin && end > begin && static_cast<std::size_t>(end - begin) <= kLargestDatagram)
        dn::notePendingDatagram(begin, static_cast<std::size_t>(end - begin));
    flush(buffer);
}

}  // namespace

std::vector<CallSite> NetTrafficCalls() {
    return {
        {"packet controller flushes a datagram (send)", 0x12CE99A, kFlush},
        {"packet controller flushes a datagram (acknowledgements)", 0x12CEDBB, kFlush},
        {"packet controller flushes a full datagram", 0x12D0020, kFlush},
    };
}

void* NetTrafficCallHandler(std::uint32_t rva) {
    for (const CallSite& call : NetTrafficCalls())
        if (call.rva == rva) return reinterpret_cast<void*>(&NetFlushHook);
    return nullptr;
}

void InitNetTraffic(const unsigned char* gameBase) {
    flush = reinterpret_cast<FlushFn>(const_cast<unsigned char*>(gameBase) + kFlush);
}

}  // namespace multislot
