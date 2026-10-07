#include "netcompress.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <atomic>
#include <cstring>

#include "log.h"
#include "netfeature.h"
#include "src/netclass.h"

namespace multislot {
namespace {

// ntdll's XPRESS (COMPRESSION_FORMAT_XPRESS): part of every Windows the game runs on.
constexpr unsigned short kFormatXpress = 3;
using GetWorkspaceFn = long(NTAPI*)(unsigned short, unsigned long*, unsigned long*);
using CompressFn = long(NTAPI*)(unsigned short, unsigned char*, unsigned long, unsigned char*, unsigned long,
                                unsigned long, unsigned long*, void*);
using DecompressFn = long(NTAPI*)(unsigned short, unsigned char*, unsigned long, unsigned char*, unsigned long,
                                  unsigned long*);
struct Xpress {
    GetWorkspaceFn workspace = nullptr;
    CompressFn compress = nullptr;
    DecompressFn decompress = nullptr;
    unsigned long workspaceSize = 0;
    bool ok = false;
};

const Xpress& Api() {
    static const Xpress api = [] {
        Xpress x;
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return x;
        x.workspace = reinterpret_cast<GetWorkspaceFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetCompressionWorkSpaceSize")));
        x.compress = reinterpret_cast<CompressFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlCompressBuffer")));
        x.decompress = reinterpret_cast<DecompressFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlDecompressBuffer")));
        unsigned long fragment = 0;
        x.ok = x.workspace && x.compress && x.decompress && x.workspace(kFormatXpress, &x.workspaceSize, &fragment) == 0;
        return x;
    }();
    return api;
}

// --- the game ---
constexpr std::uint32_t kEncrypt = 0x7B8060, kDecrypt = 0xD2360;
constexpr std::uintptr_t kOperatorNew = 0x12D85B0, kOperatorDelete = 0x12D85EC;
using CipherFn = bool(__fastcall*)(void* cipher, const std::uint8_t* data, std::size_t size);
using NewFn = void*(__fastcall*)(std::size_t);
using DeleteFn = void(__fastcall*)(void*, std::size_t);
CipherFn encrypt = nullptr, decrypt = nullptr;
NewFn gameNew = nullptr;
DeleteFn gameDelete = nullptr;
bool enabled = false;
std::atomic<std::uint64_t> packedCount{0}, savedBytes{0}, unpackedCount{0}, damagedCount{0};

// The game's std::string (MSVC: 16 bytes of inline buffer or a pointer, size, capacity), allocated with the game's
// own operator new/delete the way its std::allocator<char> does: blocks of 4096 bytes and more over-allocated by
// 0x27, aligned to 32, the raw pointer kept just before (the game frees them so, 12CEB72..12CEB97).
struct GameString {
    char storage[16];  // the characters (capacity < 16) or the pointer to them
    std::size_t size;
    std::size_t capacity;
};
static_assert(sizeof(GameString) == 32, "std::string");

char* GameStringData(GameString& s) {
    if (s.capacity < 16) return s.storage;
    char* pointer = nullptr;
    std::memcpy(&pointer, s.storage, sizeof(pointer));
    return pointer;
}

void SetGameStringPointer(GameString& s, char* pointer) { std::memcpy(s.storage, &pointer, sizeof(pointer)); }

char* GameAllocate(std::size_t bytes) {
    if (bytes < 0x1000) return static_cast<char*>(gameNew(bytes));
    auto* raw = static_cast<char*>(gameNew(bytes + 0x27));
    if (!raw) return nullptr;
    char* aligned = reinterpret_cast<char*>((reinterpret_cast<std::uintptr_t>(raw) + 0x27) & ~std::uintptr_t{0x1F});
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return aligned;
}

void GameFree(char* block, std::size_t bytes) {
    if (bytes < 0x1000) return gameDelete(block, bytes);
    gameDelete(reinterpret_cast<void**>(block)[-1], bytes + 0x27);
}

// Makes the game's string hold `data`.
bool AssignGameString(GameString& s, const std::uint8_t* data, std::size_t size) {
    if (size > s.capacity) {
        const std::size_t capacity = size | 0xF;
        char* block = GameAllocate(capacity + 1);
        if (!block) return false;
        if (s.capacity >= 16) GameFree(GameStringData(s), s.capacity + 1);
        SetGameStringPointer(s, block);
        s.capacity = capacity;
    }
    char* to = GameStringData(s);
    std::memcpy(to, data, size);
    to[size] = '\0';
    s.size = size;
    return true;
}

bool __fastcall NetEncryptHook(void* cipher, const std::uint8_t* data, std::size_t size) {
    thread_local std::vector<std::uint8_t> packed;
    if (enabled && NetFeatureActive(NetFeature::Compression) && PackPlaintext(data, size, packed)) {
        ++packedCount;
        savedBytes += size - packed.size();
        // The datagram EOS gets is the packed one: its class goes with that size (src/netclass.h).
        dn::retargetPendingDatagram(dn::kControllerDatagramHeader + packed.size());
        return encrypt(cipher, packed.data(), packed.size());
    }
    return encrypt(cipher, data, size);
}

bool __fastcall NetDecryptHook(void* cipher, const std::uint8_t* data, std::size_t size) {
    if (!decrypt(cipher, data, size)) return false;
    auto& s = *static_cast<GameString*>(cipher);
    const auto* plain = reinterpret_cast<const std::uint8_t*>(GameStringData(s));
    if (!IsPackedPlaintext(plain, s.size)) return true;
    thread_local std::vector<std::uint8_t> unpacked;
    // A damaged one is left as it is: its CRC fails and the game drops it as it drops any damaged datagram.
    if (!UnpackPlaintext(plain, s.size, unpacked) || !AssignGameString(s, unpacked.data(), unpacked.size())) {
        ++damagedCount;
        return true;
    }
    ++unpackedCount;
    return true;
}

}  // namespace

bool PackingAvailable() { return Api().ok; }

bool PackPlaintext(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    const Xpress& x = Api();
    if (!x.ok || !data || size < kPackMinimum || size > kMaxUnpacked) return false;
    thread_local std::vector<std::uint8_t> workspace;
    if (workspace.size() < x.workspaceSize) workspace.resize(x.workspaceSize);
    out.resize(kPackHeader + size);
    unsigned long packed = 0;
    if (x.compress(kFormatXpress, const_cast<unsigned char*>(data), static_cast<unsigned long>(size), out.data() + kPackHeader,
                   static_cast<unsigned long>(size), 4096, &packed, workspace.data()) != 0)
        return false;
    if (kPackHeader + packed + kPackSavingMin > size) return false;
    std::memcpy(out.data(), kPackMagic, sizeof(kPackMagic));
    out[4] = static_cast<std::uint8_t>(size);
    out[5] = static_cast<std::uint8_t>(size >> 8);
    out.resize(kPackHeader + packed);
    return true;
}

bool IsPackedPlaintext(const std::uint8_t* data, std::size_t size) {
    return data && size > kPackHeader && std::memcmp(data, kPackMagic, sizeof(kPackMagic)) == 0;
}

bool UnpackPlaintext(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out) {
    const Xpress& x = Api();
    if (!x.ok || !IsPackedPlaintext(data, size)) return false;
    const std::size_t original = static_cast<std::size_t>(data[4]) | static_cast<std::size_t>(data[5]) << 8;
    if (original == 0 || original > kMaxUnpacked) return false;
    out.resize(original);
    unsigned long got = 0;
    if (x.decompress(kFormatXpress, out.data(), static_cast<unsigned long>(original), const_cast<unsigned char*>(data) + kPackHeader,
                     static_cast<unsigned long>(size - kPackHeader), &got) != 0 ||
        got != original)
        return false;
    return true;
}

std::vector<CallSite> NetCompressCalls() {
    return {
        {"packet controller encrypts a datagram's records", 0x12CEBAA, kEncrypt},
        {"packet controller decrypts a datagram's records", 0x12CE16B, kDecrypt},
    };
}

void* NetCompressCallHandler(std::uint32_t rva) {
    if (rva == 0x12CEBAA) return reinterpret_cast<void*>(&NetEncryptHook);
    if (rva == 0x12CE16B) return reinterpret_cast<void*>(&NetDecryptHook);
    return nullptr;
}

void InitNetCompress(const unsigned char* gameBase, bool on) {
    auto* base = const_cast<unsigned char*>(gameBase);
    encrypt = reinterpret_cast<CipherFn>(base + kEncrypt);
    decrypt = reinterpret_cast<CipherFn>(base + kDecrypt);
    gameNew = reinterpret_cast<NewFn>(base + kOperatorNew);
    gameDelete = reinterpret_cast<DeleteFn>(base + kOperatorDelete);
    enabled = on && PackingAvailable();
    if (on && !enabled) Log("NETCODE Compression: XPRESS (ntdll) is not available; datagrams go unpacked");
}

void TakePackStats(std::uint64_t& packed, std::uint64_t& saved, std::uint64_t& unpacked) {
    packed = packedCount.exchange(0);
    saved = savedBytes.exchange(0);
    unpacked = unpackedCount.exchange(0);
    if (const std::uint64_t damaged = damagedCount.exchange(0))
        Log("NETCODE XPRESS: %llu packed datagrams could not be unpacked (the game drops them as damaged)",
            static_cast<unsigned long long>(damaged));
}

}  // namespace multislot
