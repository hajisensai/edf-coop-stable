#include "missionsync.h"

#include <cstring>
#include <map>
#include <memory>

#include "net_shared.h"

namespace gamenet {
namespace {

// EDF.dll (RVAs).
constexpr std::uintptr_t kGameDataMgr = 0x20B2890, kEosCore = 0x20B2AC0, kNetwork = 0x20B2AC8;
constexpr std::size_t kGameDataMgrSize = 0x150A0, kNetworkSize = 0x1720, kEosCoreSize = 0x180;
constexpr std::uintptr_t kBegin = 0x78E9A0;   // MissionSync_Begin(int id)
constexpr std::uintptr_t kUpdate = 0x790600;  // MissionSync_Update(int id, bool*)
constexpr std::uintptr_t kReceive = 0x74C570;  // Syncronize receive (map, &sender, event, envelope)
constexpr std::uintptr_t kSendToEvent = 0x74E1B0, kBroadcastEvent = 0x750130;
constexpr std::uintptr_t kSerializeNew = 0x12B4570, kSerializeFrom = 0x12B4530, kSerializeData = 0x12B4630;
constexpr std::uintptr_t kWriteInt = 0x12B5580, kWriteU8 = 0x12B5790, kWriteU16 = 0x12B54E0, kWriteBytes = 0x12B5200;
constexpr std::uintptr_t kReadInt = 0x12B4660, kReadBlock = 0x12B4900;
constexpr std::size_t kSerializeSize = 0x5F8, kSerializeRead = 0x8, kSerializeLength = 0x5F0;
// GameDataMgr fields.
constexpr std::size_t kWeaponEntries = 0x6E68, kWeaponTable = 0x130;
constexpr std::size_t kMission = 0x48, kDifficulty = 0x4C, kClass = 0x6E90, kMarker = 0x6E94, kWeapons = 0x6E98,
                      kArmor = 0x6F88, kRequestRecords = 0x14FF4, kPlayers = 0x14FF8, kRecords = 0x14C78,
                      kRecordSize = 0xD4;
// eos::Core (20B2AC0 points at +0x98), its room, eos::GameImpl, eos::User.
constexpr std::size_t kCoreRoom = 0xC0, kCoreGame = 0xD0, kRoomStays = 0x78;
constexpr std::size_t kGameLeader = 0x20, kGameUsers = 0x30, kGameSyncs = 0xF0, kGameSize = 0x120;
constexpr std::size_t kUserSlot = 0x48;
constexpr std::uint16_t kSynchronizeGate = 4;
constexpr std::uint16_t kChatterType = 7;  // any other event type: what else the game says in the same frames  // the event type of the sync messages (Event_SynchronizeGate)
// The packet controller record type the event messages travel in: Transmit::Send's ((sub & 0xF) | id << 4) << 8.
// Its value only names the record (its header is the same 12 bytes whatever it is).
constexpr std::uint32_t kEventRecordType = 0x3000;

struct IndexKey {
    std::int32_t index;
    std::int32_t zero;
};

// What the event controller would hold of one event: the fields 74C570 and the framing read, and whose it is.
struct Event {
    // +8 flags (74C570), +0xC counter, +0x10 type, +0x14 id, +0x28 the weak_ptr of enable_shared_from_this (74C570
    // keeps the event a message came with as a shared_ptr: the one its answers go to)
    std::uint8_t game[0x48];
    std::int32_t owner;       // network index of the machine that created it
    std::int32_t id;
};

struct State {
    Transport* transport = nullptr;
    MissionSync* sync = nullptr;
    std::vector<std::string> members;
    int self = -1;  // network index
    std::vector<std::shared_ptr<Event>> events;
    std::int32_t nextId = 1;
    std::map<std::string, std::uint8_t*> batches;  // the event controller's per-peer builders
    std::map<std::string, bool> chatterWaiting;    // a builder holds a background message
    Event* chatter = nullptr;
    std::uint8_t* gameImpl = nullptr;
} state;

const Game& G() { return state.transport->game(); }

std::uint8_t* NewSerialize() {
    auto* stream = static_cast<std::uint8_t*>(G().New(kSerializeSize));
    G().Fn<void (*)(void*, int)>(kSerializeNew)(stream, 0x40);
    return stream;
}

Event* NewEvent(std::int32_t owner, std::int32_t id, std::uint16_t type) {
    auto shared = std::make_shared<Event>();
    Event* event = shared.get();
    std::memset(event, 0, sizeof(Event));
    Shared self{};
    std::memcpy(&self, &shared, sizeof(self));  // std::shared_ptr is {object, control block}
    std::memcpy(event->game + 0x28, &self, sizeof(self));  // enable_shared_from_this's weak_ptr
    _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(self.control) + 0xC));
    state.events.push_back(std::move(shared));
    event->owner = owner;
    event->id = id;
    std::memcpy(event->game + 0x10, &type, sizeof(type));
    std::memcpy(event->game + 0x14, &id, sizeof(id));
    return event;
}

Event* FindEvent(std::int32_t owner, std::int32_t id) {
    for (const auto& event : state.events)
        if (event->owner == owner && event->id == id) return event.get();
    return nullptr;
}

std::string MemberAt(int index) {
    for (const auto& member : state.members)
        if (state.transport->NetworkIndex(member) == index) return member;
    return {};
}

// The per-peer builder of the event controller (761E60): messages to a member are appended to its batch, which
// goes to the packet controller as one record once it holds more than 250 bytes, and at the end of the frame.
constexpr std::size_t kBatchSendAbove = 0xFA;  // 74ED6D

void FlushBatch(const std::string& to, std::uint8_t*& batch) {
    const void* bytes = nullptr;
    std::size_t length = 0;
    G().Fn<bool (*)(void*, const void**, std::size_t*)>(kSerializeData)(batch, &bytes, &length);
    if (length && !state.transport->SendReliable(to, kEventRecordType, bytes, length))
        Result("event-send", "the controller refused %zu bytes to %s", length, to.c_str());
    batch = NewSerialize();
    state.chatterWaiting[to] = false;
}

// 750380's framing of one message to an event, appended to the batch for `to`.
void SendFramed(const Event& event, const void* data, std::size_t size, const std::string& to) {
    std::uint16_t type = 0;
    std::uint32_t counter = 0;
    std::memcpy(&type, event.game + 0x10, sizeof(type));
    std::memcpy(&counter, event.game + 0xC, sizeof(counter));
    if (type == kSynchronizeGate) Result("trace", "event %d: %zu bytes to %s", event.id, size, to.c_str());
    std::uint8_t*& batch = state.batches[to];
    if (!batch) batch = NewSerialize();
    G().Fn<bool (*)(void*, int)>(kWriteU16)(batch, type);
    G().Fn<bool (*)(void*, int)>(kWriteInt)(batch, -event.id);
    G().Fn<bool (*)(void*, int)>(kWriteU8)(batch, static_cast<std::uint8_t>(counter));
    G().Fn<bool (*)(void*, const void*, std::size_t)>(kWriteBytes)(batch, data, size);
    const std::size_t length = *reinterpret_cast<std::size_t*>(batch + kSerializeLength);
    if (type == kChatterType) state.chatterWaiting[to] = true;
    else if (state.chatterWaiting[to] && size > kBatchSendAbove)
        Result("shared-batch", "%zu bytes to %s: a %zu-byte sync message after a background one", length, to.c_str(), size);
    if (length > kBatchSendAbove) FlushBatch(to, batch);
}

// Sends to an event: the creator's message reaches everyone it was broadcast to (`only`: one of them), anyone
// else's its creator.
void SendToEvent(Event& event, const void* data, std::size_t size, int only = -1) {
    ++*reinterpret_cast<std::uint32_t*>(event.game + 0xC);
    if (event.owner != state.self) return SendFramed(event, data, size, MemberAt(event.owner));
    for (const auto& member : state.members) {
        const int index = state.transport->NetworkIndex(member);
        if (index != state.self && (only < 0 || only == index)) SendFramed(event, data, size, member);
    }
}

// 74E1B0: one message to an event.
bool __fastcall HookSendToEvent(void* event, const void* data, std::size_t size) {
    SendToEvent(*static_cast<Event*>(event), data, size);
    return true;
}

// Event ids name their creator: id % kMaxMachines is its network index (the game's own numbering is not known;
// only that both ends of an event agree on it).
std::int32_t NextId() { return state.nextId++ * gamenet::kMaxMachines + state.self; }

// 750130: a new event of `type`, broadcast to `to` (index -1: the whole room), with its first message.
Shared* __fastcall HookBroadcastEvent(void*, Shared* out, std::int32_t type, const void* data, std::size_t size,
                                      const IndexKey* to) {
    Event* event = NewEvent(state.self, NextId(), static_cast<std::uint16_t>(type));
    SendToEvent(*event, data, size, to ? to->index : -1);
    Shared self{};
    std::memcpy(&self, event->game + 0x28, sizeof(self));
    *out = AddRef(self);  // the caller lets go of it
    return out;
}

// The event controller's receive: unframes every message of a controller record and hands the sync ones to
// the game's receive (74C570), with the event they belong to.
void OnEventRecord(int from, const std::uint8_t* data, std::size_t size) {
    std::uint8_t* stream = static_cast<std::uint8_t*>(G().New(kSerializeSize));
    G().Fn<void (*)(void*, const void*, std::size_t)>(kSerializeFrom)(stream, data, size);
    const auto position = [&] { return *reinterpret_cast<std::size_t*>(stream + kSerializeRead); };
    while (position() < size) {
        const auto type = static_cast<std::uint16_t>(G().Fn<std::int64_t (*)(void*)>(kReadInt)(stream));
        std::int32_t id = static_cast<std::int32_t>(G().Fn<std::int64_t (*)(void*)>(kReadInt)(stream));
        G().Fn<std::int64_t (*)(void*)>(kReadInt)(stream);  // the counter
        std::uint8_t* envelope = NewSerialize();
        if (!G().Fn<bool (*)(void*, void*)>(kReadBlock)(stream, envelope)) {
            Result("event-receive", "a malformed event message from %d", from);
            return;
        }
        id = id < 0 ? -id : id;
        if (type != kSynchronizeGate) continue;  // background messages (Chatter)
        const std::int32_t owner = id % gamenet::kMaxMachines;
        Event* event = FindEvent(owner, id);
        if (!event) event = NewEvent(owner, id, type);
        Result("trace", "event %d: a message from %d", id, from);
        IndexKey sender{from, 0};
        G().Fn<void (*)(void*, IndexKey*, void*, void*)>(kReceive)(state.gameImpl + kGameSyncs, &sender, event, envelope);
    }
}

// eos::GameImpl's virtual GetUsers (slot 1): the room's users as shared_ptrs, in a vector it constructs.
void* __fastcall GetUsers(void*, void* out) {
    std::vector<Shared> users;
    for (const auto& member : state.members) {
        users.push_back(AddRef(state.transport->UserShared(member)));  // the game's eos::User
    }
    auto* array = static_cast<Shared*>(G().New(sizeof(Shared) * users.size()));
    std::memcpy(array, users.data(), sizeof(Shared) * users.size());
    auto** vector = static_cast<Shared**>(out);
    vector[0] = array;
    vector[1] = array + users.size();
    vector[2] = array + users.size();
    return out;
}

template <int Slot>
void __fastcall UnexpectedVirtual() {
    Result("gameimpl", "the game called eos::GameImpl virtual slot %d, which this test does not stand in for", Slot);
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
}

void* const kGameImplVtable[] = {
    reinterpret_cast<void*>(&UnexpectedVirtual<0>),  reinterpret_cast<void*>(&GetUsers),
    reinterpret_cast<void*>(&UnexpectedVirtual<2>),  reinterpret_cast<void*>(&UnexpectedVirtual<3>),
    reinterpret_cast<void*>(&UnexpectedVirtual<4>),  reinterpret_cast<void*>(&UnexpectedVirtual<5>),
    reinterpret_cast<void*>(&UnexpectedVirtual<6>),  reinterpret_cast<void*>(&UnexpectedVirtual<7>),
    reinterpret_cast<void*>(&UnexpectedVirtual<8>),  reinterpret_cast<void*>(&UnexpectedVirtual<9>),
    reinterpret_cast<void*>(&UnexpectedVirtual<10>), reinterpret_cast<void*>(&UnexpectedVirtual<11>),
    reinterpret_cast<void*>(&UnexpectedVirtual<12>), reinterpret_cast<void*>(&UnexpectedVirtual<13>),
    reinterpret_cast<void*>(&UnexpectedVirtual<14>), reinterpret_cast<void*>(&UnexpectedVirtual<15>),
};

// Points the function at `rva` to `target` (an absolute jump over its first 14 bytes).
bool Redirect(const Game& game, std::uintptr_t rva, void* target) {
    auto* code = game.Fn<std::uint8_t*>(rva);
    DWORD old = 0;
    if (!VirtualProtect(code, 14, PAGE_EXECUTE_READWRITE, &old)) return false;
    const std::uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
    std::memcpy(code, jump, sizeof(jump));
    std::memcpy(code + 6, &target, sizeof(target));
    VirtualProtect(code, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, 14);
    return true;
}

template <typename T>
void Put(std::uint8_t* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

}  // namespace

bool MissionSync::Build(Transport& transport, const std::vector<std::string>& members, const Loadout& loadout,
                        std::int32_t mission, std::int32_t difficulty) {
    transport_ = &transport;
    state.transport = &transport;
    state.sync = this;
    state.members = members;
    const Game& game = transport.game();
    state.self = transport.NetworkIndex(game.machine->user);

    // Each user's player slot is its place in the lobby, as the room gives it.
    for (std::size_t i = 0; i < members.size(); ++i)
        Put<std::int32_t>(static_cast<std::uint8_t*>(transport.User(members[i])), kUserSlot, static_cast<std::int32_t>(i));

    gdm_ = static_cast<std::uint8_t*>(game.New(kGameDataMgrSize));
    std::memset(gdm_ + kRecords, 0xEE, 4 * kRecordSize);  // a record nobody wrote shows up
    Put<std::int32_t>(gdm_, kMission, mission);
    Put<std::int32_t>(gdm_, kDifficulty, difficulty);
    Put<std::int32_t>(gdm_, kClass, loadout.soldierClass);
    Put<std::int32_t>(gdm_, kMarker, loadout.marker);
    // Shaped like a real loadout, so the record has a real record's size (about 143 bytes; zeros would encode
    // smaller): six weapons with their 8-byte entries (MissionSync_Begin reads GameDataMgr+0x6E68 + (id+0xA69)*12),
    // and eight colours (four pairs, 78EC53).
    for (int weapon = 0; weapon < 6; ++weapon) {
        const std::int32_t id = loadout.firstWeapon + 11 * weapon;
        Put<std::int32_t>(gdm_, kWeapons + loadout.soldierClass * 24 + weapon * 4, id);
        Put<std::uint64_t>(gdm_, kWeaponEntries + (static_cast<std::size_t>(id) + 0xA69) * 12,
                           0x0001000000000000ull * (weapon + 1) + 1000ull * (loadout.marker + 1) + weapon);
    }
    for (int pair = 0; pair < 4; ++pair) {
        const std::size_t at = static_cast<std::size_t>(loadout.soldierClass) * 0x620 + 0xC4 + pair * 0x188;
        const float first[4] = {0.11f * (pair + 1), 0.37f + 0.01f * loadout.marker, 0.73f, 1.0f};
        const float second[4] = {0.9f - 0.1f * pair, 0.25f, 0.5f + 0.001f * loadout.marker, 1.0f};
        std::memcpy(gdm_ + at + 0x6EEC, first, sizeof(first));  // colour index 0 of each pair
        std::memcpy(gdm_ + at + 0x6FB0, second, sizeof(second));
    }
    Put<std::int32_t>(gdm_, kArmor + loadout.soldierClass * 4, loadout.armor);
    Put<std::int32_t>(gdm_, kRequestRecords, 1);
    // The WEAPONTABLE as far as its row count (E23F0 on GameDataMgr+0x130: the table's SGO document at +0x188,
    // its node index at +0x190; the node's entry holds the rows at +0x58), which EDF6Coop's weapon guard asks for.
    auto* rows = static_cast<std::uint8_t*>(game.New(0x60));
    Put<std::uint32_t>(rows, 0x58, loadout.weaponRows);
    auto* entry = static_cast<std::uint8_t*>(game.New(40));
    Put<void*>(entry, 0, rows);
    auto* index = static_cast<std::uint8_t*>(game.New(4));
    auto* document = static_cast<std::uint8_t*>(game.New(0x60));
    Put<void*>(document, 8, index);
    Put<std::uint64_t>(document, 0x18, 1);
    Put<void*>(document, 0x28, entry);
    Put<void*>(gdm_, kWeaponTable + 0x188, document);
    Put<std::uint32_t>(gdm_, kWeaponTable + 0x190, 0);
    *game.Fn<void**>(kGameDataMgr) = gdm_;
    *game.Fn<void**>(kNetwork) = game.New(kNetworkSize);

    auto* core = static_cast<std::uint8_t*>(game.New(kEosCoreSize));
    auto* room = static_cast<std::uint8_t*>(game.New(0x100));
    Put<std::int32_t>(room, kRoomStays, 1);
    Put<void*>(core, kCoreRoom, room);
    gameImpl_ = static_cast<std::uint8_t*>(game.New(kGameSize));
    state.gameImpl = gameImpl_;
    Put<const void*>(gameImpl_, 0, kGameImplVtable);
    const Shared leader = transport.UserShared(members.front());  // the room's owner hosts the mission (a weak_ptr)
    Put<void*>(gameImpl_, kGameLeader, leader.object);
    Put<void*>(gameImpl_, kGameLeader + 8, leader.control);
    auto* users = static_cast<std::uint8_t*>(game.New(0x40));  // +0x18 the local user (734640)
    const Shared self = AddRef(transport.UserShared(game.machine->user));
    Put<void*>(users, 0x18, self.object);
    Put<void*>(users, 0x20, self.control);
    Put<void*>(gameImpl_, kGameUsers, users);
    auto* head = static_cast<std::uint8_t*>(game.New(0x38));  // std::map<int64, shared_ptr<Object>>: empty
    Put<void*>(head, 0, head);
    Put<void*>(head, 8, head);
    Put<void*>(head, 0x10, head);
    Put<std::uint16_t>(head, 0x18, 0x0101);
    Put<void*>(gameImpl_, kGameSyncs, head);
    Put<void*>(core, kCoreGame, gameImpl_);
    *game.Fn<void**>(kEosCore) = core + 0x98;

    transport.Subscribe(kEventRecordType, &OnEventRecord);
    return Redirect(game, kSendToEvent, reinterpret_cast<void*>(&HookSendToEvent)) &&
           Redirect(game, kBroadcastEvent, reinterpret_cast<void*>(&HookBroadcastEvent));
}

void MissionSync::Dump() const {
    for (const auto& member : state.members) {
        const auto* user = static_cast<const std::uint8_t*>(transport_->User(member));
        Result("dump", "user %s flags=%x index=%d slot=%d", member.c_str(), *reinterpret_cast<const std::uint32_t*>(user + 0x10),
               *reinterpret_cast<const std::int32_t*>(user + 0x40), *reinterpret_cast<const std::int32_t*>(user + 0x48));
    }
    auto* head = *reinterpret_cast<std::uint8_t**>(gameImpl_ + kGameSyncs);
    const std::uint64_t count = *reinterpret_cast<std::uint64_t*>(gameImpl_ + kGameSyncs + 8);
    Result("dump", "%llu sync object(s)", count);
    std::uint8_t* node = *reinterpret_cast<std::uint8_t**>(head + 8);  // the root
    if (node == head) return;
    std::int64_t key = 0;
    std::memcpy(&key, node + 0x20, 8);
    auto* object = *reinterpret_cast<std::uint8_t**>(node + 0x28);
    Result("dump", "object key=%llx done=%d replyEvent=%p isHost=%d request=%p response=%p", key, object[0x10],
           *reinterpret_cast<void**>(object + 0x18), object[0x68], *reinterpret_cast<void**>(object + 0x38),
           *reinterpret_cast<void**>(object + 0x48));
    auto* list = *reinterpret_cast<std::uint8_t**>(object + 0x28);
    for (auto* n = *reinterpret_cast<std::uint8_t**>(list); n != list; n = *reinterpret_cast<std::uint8_t**>(n))
        Result("dump", "node key=%d serialize=%p", *reinterpret_cast<std::int32_t*>(n + 0x10), *reinterpret_cast<void**>(n + 0x18));
}

void MissionSync::EndFrame() const {
    for (auto& [to, batch] : state.batches)
        if (batch && *reinterpret_cast<std::size_t*>(batch + kSerializeLength)) FlushBatch(to, batch);
}

void MissionSync::Chatter(std::size_t bytes) const {
    if (!state.chatter) state.chatter = NewEvent(state.self, NextId(), kChatterType);
    const std::vector<std::uint8_t> payload(bytes, 0x5C);
    SendToEvent(*state.chatter, payload.data(), payload.size());
}

void MissionSync::Begin(std::int32_t id) const { transport_->game().Fn<std::int32_t (*)(std::int32_t)>(kBegin)(id); }

std::int32_t MissionSync::Update(std::int32_t id) const {
    bool flag = false;
    return transport_->game().Fn<std::int32_t (*)(std::int32_t, bool*)>(kUpdate)(id, &flag);
}

std::int32_t MissionSync::Players() const { return *reinterpret_cast<const std::int32_t*>(gdm_ + kPlayers); }
std::int32_t MissionSync::Mission() const { return *reinterpret_cast<const std::int32_t*>(gdm_ + kMission); }
std::int32_t MissionSync::Difficulty() const { return *reinterpret_cast<const std::int32_t*>(gdm_ + kDifficulty); }

std::vector<std::uint8_t> MissionSync::Record(int slot) const {
    using RecordFn = const std::uint8_t* (*)(int);
    const HMODULE plugin = transport_->game().machine->plugin;
    const auto record = plugin ? reinterpret_cast<RecordFn>(GetProcAddress(plugin, "EDF6Coop_LoadoutRecord")) : nullptr;
    // Without EDF6Coop, the game's own four records.
    const std::uint8_t* at = record ? record(slot) : slot < 4 ? gdm_ + kRecords + slot * kRecordSize : nullptr;
    return at ? std::vector<std::uint8_t>(at, at + kRecordSize) : std::vector<std::uint8_t>();
}

}  // namespace gamenet
