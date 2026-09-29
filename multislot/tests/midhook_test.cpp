// Runs hooked machine code in this process: the thunk must hand the handler the live registers,
// load its changes back, preserve flags and xmm registers, keep the stack aligned, run the displaced
// instructions and return to the instruction after the site.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/code.h"
#include "../src/midhook.h"

using namespace multislot;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

struct Buffer {
    unsigned char* code = nullptr;
    Buffer() { code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)); }
    ~Buffer() {
        if (code) VirtualFree(code, 0, MEM_RELEASE);
    }
};

std::uint64_t alignment = 0xFF;
std::uint64_t seenRcx = 0;

void TenfoldRcx(CpuContext* context) {
    alignment = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress()) & 0xF;
    seenRcx = context->rcx;
    context->rcx *= 10;
}

void DoubleIsOneAndHalf(CpuContext* context) {
    const double value = 1.5;
    std::memcpy(context->xmm[0], &value, sizeof(value));
}

volatile double scratch = 0;
void ClobberFlagsAndXmm(CpuContext* context) {
    // Arithmetic in the handler changes the flags and xmm registers; the game must not see that.
    scratch = static_cast<double>(context->rax) * 3.25 + 1.0;
    volatile int zero = 0;
    context->rdx = static_cast<std::uint64_t>(zero - 1 < 0);
}

// Where each frame of the call stack is, walked from inside a handler the way crashlog.cpp's Stack() walks a
// crash: the unwind data where there is some, the return address on top of the stack where there is none.
DWORD64 walked[4]{};
__declspec(noinline) void WalkFromHandler(CpuContext*) {
    CONTEXT context{};
    RtlCaptureContext(&context);
    for (int frame = 0; frame < 4 && context.Rip; ++frame) {
        walked[frame] = context.Rip;
        DWORD64 imageBase = 0;
        const auto function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher,
                             nullptr);
        } else {
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += 8;
        }
    }
}

void FaultingHandler(CpuContext*) {
    volatile int* nowhere = nullptr;
    *nowhere = 1;
}

// The system has to unwind from the handler, through the thunk and the hooked function, to reach this __except.
bool FaultReachesCaller(int (*hooked)()) {
    __try {
        hooked();
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

bool InThisModule(DWORD64 address) {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(static_cast<std::uintptr_t>(address)), &module) &&
           module == GetModuleHandleW(nullptr);
}

int Seven() { return 7; }

// Installs a hook over `length` bytes at `site` whose displaced instructions are the last
// `displacedSize` bytes of the site.
bool Hook(ThunkPage& page, unsigned char* site, std::size_t length, std::size_t displacedSize, MidHandler handler) {
    std::vector<std::uint8_t> original(site, site + length);
    const auto resume = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(site + length));
    unsigned char* thunk = EmitMidThunk(page, handler, original.data() + length - displacedSize, displacedSize, resume);
    if (!thunk) return false;
    const auto jump = JumpBytes(site, thunk, length);
    if (jump.empty()) return false;
    std::memcpy(site, jump.data(), jump.size());
    return true;
}

}  // namespace

int main() {
    Buffer buffer;
    Check(buffer.code != nullptr, "code buffer");
    if (!buffer.code) return 1;
    ThunkPage page;
    Check(page.Allocate(buffer.code), "thunk page near the code buffer");

    // f(x): mov rax, rcx; add rax, 5; ret   (site = both instructions, both displaced)
    unsigned char* f = buffer.code;
    const unsigned char fCode[] = {0x48, 0x89, 0xC8, 0x48, 0x83, 0xC0, 0x05, 0xC3};
    std::memcpy(f, fCode, sizeof(fCode));
    Check(Hook(page, f, 7, 7, &TenfoldRcx), "hook f");

    // g(x): addsd xmm0, xmm0; nop; ret   (displaced addsd runs after the handler set xmm0 = 1.5)
    unsigned char* g = buffer.code + 0x20;
    const unsigned char gCode[] = {0xF2, 0x0F, 0x58, 0xC0, 0x90, 0xC3};
    std::memcpy(g, gCode, sizeof(gCode));
    Check(Hook(page, g, 5, 5, &DoubleIsOneAndHalf), "hook g");

    // h(): stc; [nop x5 site]; setc al; movzx eax, al; ret   (carry must survive the thunk)
    unsigned char* h = buffer.code + 0x40;
    const unsigned char hCode[] = {0xF9, 0x90, 0x90, 0x90, 0x90, 0x90, 0x0F, 0x92, 0xC0, 0x0F, 0xB6, 0xC0, 0xC3};
    std::memcpy(h, hCode, sizeof(hCode));
    Check(Hook(page, h + 1, 5, 5, &ClobberFlagsAndXmm), "hook h");

    // k(x): mov eax, 1 (replaced, not displaced); mov edx, 2 (displaced); add eax, edx; ret
    unsigned char* k = buffer.code + 0x60;
    const unsigned char kCode[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xBA, 0x02, 0x00, 0x00, 0x00, 0x01, 0xD0, 0xC3};
    std::memcpy(k, kCode, sizeof(kCode));
    Check(Hook(page, k, 10, 5, [](CpuContext* context) { context->rax = 40; }), "hook k");

    // m(x, y): movaps xmm1-ish check: y (xmm1) must come back untouched through a handler that clobbers xmm
    // movapd xmm0, xmm1 (66 0F 28 C1) + nop; ret
    unsigned char* m = buffer.code + 0x80;
    const unsigned char mCode[] = {0x66, 0x0F, 0x28, 0xC1, 0x90, 0xC3};
    std::memcpy(m, mCode, sizeof(mCode));
    Check(Hook(page, m, 5, 5, &ClobberFlagsAndXmm), "hook m");

    // p(): 5 nops (the site, displaced); ret. Its handler walks the call stack from inside.
    unsigned char* p = buffer.code + 0xA0;
    const unsigned char pCode[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
    std::memcpy(p, pCode, sizeof(pCode));
    Check(Hook(page, p, 5, 5, &WalkFromHandler), "hook p");
    unsigned char* const pThunk = p + 5 + *reinterpret_cast<const std::int32_t*>(p + 1);

    // q(): 5 nops (the site); mov eax, 7; ret. Its handler faults.
    unsigned char* q = buffer.code + 0xC0;
    const unsigned char qCode[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(q, qCode, sizeof(qCode));
    Check(Hook(page, q, 5, 5, &FaultingHandler), "hook q");

    // A call stub, as the redirected calls use.
    unsigned char* stub = page.Add(reinterpret_cast<void*>(&Seven));
    Check(stub != nullptr, "stub emitted");

    Check(page.Seal(), "seal thunk page");
    Check(page.Unwindable(), "the page's function table is registered");

    // The thunks and stubs are described to the unwinder.
    DWORD64 imageBase = 0;
    const auto thunkEntry = RtlLookupFunctionEntry(static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(pThunk) + 60),
                                                   &imageBase, nullptr);
    Check(thunkEntry && imageBase == static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(page.Base())) &&
              page.Base() + thunkEntry->BeginAddress == pThunk && thunkEntry->EndAddress - thunkEntry->BeginAddress == MidThunkFrameBytes(),
          "a thunk's frame is in the page's function table");
    Check(RtlLookupFunctionEntry(static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(stub)), &imageBase, nullptr) != nullptr,
          "so is a stub");
    Check(reinterpret_cast<int (*)()>(stub)() == 7, "the stub still reaches its target");
    const auto layout = MidThunkCode(&TenfoldRcx, nullptr, 0, 0);
    const unsigned char drop[] = {0x48, 0x8D, 0x64, 0x24, 0x08};  // lea rsp, [rsp+8]
    Check(layout.size() > MidThunkFrameBytes() + sizeof(drop) &&
              std::memcmp(layout.data() + MidThunkFrameBytes(), drop, sizeof(drop)) == 0,
          "the described frame ends where the thunk drops the pushed resume address");

    // Walked from the handler: the handler, the thunk, the hooked function at the site's resume address, and the
    // caller of that function in this executable. Without unwind data the walk lost its way at the thunk.
    reinterpret_cast<void (*)()>(p)();
    Check(InThisModule(walked[0]), "frame 0 is the handler");
    Check(page.Contains(reinterpret_cast<const unsigned char*>(static_cast<std::uintptr_t>(walked[1]))), "frame 1 is the thunk");
    Check(walked[2] == static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(p + 5)), "frame 2 is the hooked function, at the site");
    Check(InThisModule(walked[3]), "frame 3 is the function that called it");

    // A fault in a handler is dispatched through the thunk to an __except above the hooked function.
    Check(FaultReachesCaller(reinterpret_cast<int (*)()>(q)), "a handler's fault unwinds through the thunk to the caller's handler");

    const auto fn = reinterpret_cast<std::uint64_t(*)(std::uint64_t)>(f);
    Check(fn(4) == 45, "handler sees rcx and its change reaches the displaced instructions");
    Check(seenRcx == 4, "handler saw the argument register");
    Check(alignment == 8, "handler runs on a 16-byte aligned stack");
    const auto gn = reinterpret_cast<double(*)(double)>(g);
    Check(gn(100.0) == 3.0, "xmm0 written by the handler is loaded back");
    const auto hn = reinterpret_cast<int(*)()>(h);
    Check(hn() == 1, "flags survive the thunk");
    const auto kn = reinterpret_cast<int(*)()>(k);
    Check(kn() == 42, "replaced instruction is emulated, displaced one still runs");
    const auto mn = reinterpret_cast<double(*)(double, double)>(m);
    Check(mn(1.0, 7.25) == 7.25, "xmm1 survives a handler that uses floating point");

    const auto code = MidThunkCode(&TenfoldRcx, nullptr, 0, 0x1122334455667788ull);
    Check(code.size() > 20 && code[code.size() - 14] == 0xFF && code[code.size() - 13] == 0x25, "thunk ends in jmp [rip+0]");
    std::uint64_t resume = 0;
    std::memcpy(&resume, code.data() + code.size() - 8, 8);
    Check(resume == 0x1122334455667788ull, "thunk resume address");

    page.Release();
    Check(RtlLookupFunctionEntry(static_cast<DWORD64>(reinterpret_cast<std::uintptr_t>(pThunk)), &imageBase, nullptr) == nullptr,
          "releasing the page removes its function table");
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("mid-function hooks verified\n");
    return 0;
}
