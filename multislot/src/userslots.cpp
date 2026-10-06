#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "userslots.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "crashlog.h"
#include "log.h"

namespace multislot {
namespace {

std::atomic<HostSlotFn> hostSlotOf{nullptr};
std::atomic<IdTextFn> idTextOf{nullptr};

std::mutex tableMutex;
std::uint64_t tableUsers = 0;     // the eos::Users the table is of (one per room)
std::vector<std::string> table;  // by slot

void Note(std::uint64_t users, std::size_t slot, const std::string& member) {
    std::lock_guard<std::mutex> lock(tableMutex);
    if (users != tableUsers) {  // another room's
        tableUsers = users;
        table.clear();
    }
    if (slot >= table.size()) table.resize(slot + 1);
    table[slot] = member;
}

std::string MemberText(const void* puid) {
    const IdTextFn text = idTextOf.load();
    char id[64]{};
    return text && puid ? std::string(text(puid, id, sizeof(id))) : std::string();
}

template <typename T>
bool Read(std::uint64_t address, T* out) {
    return Probing([&] {
        __try {
            std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

// Users::Add (12B7F50) after its search for the first empty slot, `movsxd rax, ecx; cmp rax, r8` (run after this):
// ecx the slot found (r8 when none is), r8 the slot count, r9 the slots (shared_ptr<User>, 16 bytes each), r13 the
// Users, rbx the {ProductUserId, remote} argument; the slot is also kept at [rbp], where the User's constructor
// reads it.
void AddHandler(CpuContext* context) {
    const int firstEmpty = static_cast<int>(static_cast<std::int32_t>(context->rcx));
    const std::size_t slots = static_cast<std::size_t>(context->r8);
    const void* puid = nullptr;
    if (!Read(context->rbx, &puid)) return;
    const std::string member = MemberText(puid);
    const HostSlotFn source = hostSlotOf.load();
    const int host = source && !member.empty() ? source(member) : -1;
    bool hostEmpty = false;
    if (host >= 0 && static_cast<std::size_t>(host) < slots) {
        std::uint64_t user = 0;
        hostEmpty = Read(context->r9 + static_cast<std::uint64_t>(host) * 16, &user) && user == 0;
    }
    const int slot = ChooseUserSlot(firstEmpty, host, slots, hostEmpty);
    if (slot != firstEmpty) {
        const auto index = static_cast<std::int32_t>(slot);
        context->rcx = static_cast<std::uint32_t>(index);
        Probing([&] {
            __try {
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(context->rbp)), &index, sizeof(index));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        });
        Log("ROOM member %s takes slot %d, the one the room's host's game has it in (the first empty one here: %d)",
            member.c_str(), slot, firstEmpty);
    } else if (host >= 0 && host != slot) {
        Log("ROOM member %s: the room's host's game has it in slot %d, which is %s here; it takes slot %d", member.c_str(),
            host, static_cast<std::size_t>(host) < slots ? "taken" : "past the table", slot);
    }
    if (slot >= 0 && static_cast<std::size_t>(slot) < slots) Note(context->r13, static_cast<std::size_t>(slot), member);
}

// Users::Remove (12B87C0) as it empties the slot, `mov rax, rbp; shl rax, 4` (run after this): rbp the slot, r15 the
// Users.
void RemoveHandler(CpuContext* context) {
    std::lock_guard<std::mutex> lock(tableMutex);
    const auto slot = static_cast<std::size_t>(context->rbp);
    if (context->r15 == tableUsers && slot < table.size()) table[slot].clear();
}

}  // namespace

void SetUserSlotSources(HostSlotFn hostSlot, IdTextFn idText) {
    hostSlotOf = hostSlot;
    idTextOf = idText;
}

int ChooseUserSlot(int firstEmpty, int hostSlot, std::size_t slots, bool hostSlotEmpty) {
    if (hostSlot >= 0 && static_cast<std::size_t>(hostSlot) < slots && hostSlotEmpty) return hostSlot;
    return firstEmpty;
}

std::vector<std::string> GameSlotTable() {
    std::lock_guard<std::mutex> lock(tableMutex);
    std::vector<std::string> out = table;
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

MidHandler UserSlotHookHandler(std::uint32_t rva) {
    switch (rva) {
        case 0x12B806E: return &AddHandler;
        case 0x12B8A24: return &RemoveHandler;
        default: return nullptr;
    }
}

}  // namespace multislot
