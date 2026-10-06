#pragma once
#include <cstdint>
#include <vector>

#include "patches.h"

namespace multislot {

// The game's datagrams in plaintext, as its packet controller flushes them (src/netclass.h): the three calls of
// the flush 12CEA10 (from the controller's send path 12CE99A, its receive path's acknowledgements 12CEDBB and the
// per-peer builder 12D0020) go through NetFlushHook, which hands the records to the direct-link part's EOS send
// hook before the game encrypts them. It reads and changes nothing of the game's.
std::vector<CallSite> NetTrafficCalls();
void* NetTrafficCallHandler(std::uint32_t rva);
void InitNetTraffic(const unsigned char* gameBase);

}  // namespace multislot
