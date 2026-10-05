#include "game.h"

#include <cstring>
#include <new>

namespace gamenet {
namespace {

// EDF.dll (RVAs).
constexpr std::uintptr_t kOperatorNew = 0x12D85B0;
constexpr std::uintptr_t kInternalCoreVtable = 0x17EBBB0;  // eos::internal_Core
constexpr std::uintptr_t kObservableCtor = 0x7267C0;       // ev::Observable (0x28 bytes)
constexpr std::uintptr_t kCoreUpdate = 0x12ADB30;          // internal_Core::Update
constexpr std::uintptr_t kUsersRef = 0x1AF4548, kUsersCtor = 0x12B77E0, kUsersSize = 0x150;
constexpr std::uintptr_t kUsersAdd = 0x12B7F50;
constexpr std::uintptr_t kManagerRef = 0x1AF4570, kManagerCtor = 0x12C6280, kManagerSize = 0x218;
constexpr std::uintptr_t kManagerInitialize = 0x12C80B0;
constexpr std::uintptr_t kEpicIdRef = 0x1AF42A8, kEpicIdCtor = 0x12B5C20, kEpicIdSize = 0x48;  // PlatformId::ID_EPIC
constexpr std::uintptr_t kControllerRef = 0x1AF4598, kControllerCtor = 0x12D2640, kControllerSize = 0x18B8;
constexpr std::uintptr_t kConnectInfoCtor = 0x12B6030;
constexpr std::uintptr_t kSubscribe = 0x735480;          // Controller::DataEventObservable (at +0x1818)
constexpr std::uintptr_t kDataObservable = 0x1818;
constexpr std::uintptr_t kSendReliable = 0x12D0AC0;
constexpr std::uintptr_t kUserFlags = 0x10, kUserNetworkIndex = 0x40;

// Room+0x30 in the game: the local user as p2p::Manager::Initialize takes it.
struct LocalUser {
    Shared id;           // PlatformId
    std::wstring name;
    const void* puid;    // EOS_ProductUserId
};
static_assert(offsetof(LocalUser, puid) == 0x30, "LocalUser layout");

// What eos::Users::Add takes (built at Room::Initialize 12BDF83).
struct ConnectInfo {
    Shared id;
    std::wstring name;
};
struct UserDesc {
    const void* puid;
    bool remote;
    ConnectInfo info;
};
static_assert(offsetof(UserDesc, info) == 0x10 && sizeof(ConnectInfo) == 0x30, "UserDesc layout");

struct IndexKey {  // eos::UsrIndexKey
    std::int32_t index;
    std::int32_t zero;
};

const void* Puid(const std::string& text) {
    using UserFn = const void* (*)(const char*);
    static const auto user = reinterpret_cast<UserFn>(
        GetProcAddress(GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll"), "FakeNet_User"));
    return user(text.c_str());
}

// A std::function the callee takes by value (and destroys): built in place, never destroyed here.
template <typename F, typename L>
F* ByValue(L&& lambda) {
    return new F(std::forward<L>(lambda));  // the callee destroys the function; its storage is leaked, harmlessly
}

}  // namespace

Shared AddRef(const Shared& shared) {
    if (shared.control) _InterlockedIncrement(reinterpret_cast<volatile long*>(static_cast<char*>(shared.control) + 8));
    return shared;
}

void* Game::New(std::size_t size) const {
    void* memory = Fn<void* (*)(std::size_t)>(kOperatorNew)(size);
    std::memset(memory, 0, size);
    return memory;
}

Shared Game::MakeShared(std::size_t objectSize, std::uintptr_t refVtable) const {
    auto* block = static_cast<std::uint8_t*>(New(0x10 + objectSize));
    *reinterpret_cast<void**>(block) = Fn<void*>(refVtable);
    *reinterpret_cast<std::int32_t*>(block + 8) = 1;
    *reinterpret_cast<std::int32_t*>(block + 0xC) = 1;
    return Shared{block + 0x10, block};
}

bool Transport::Start(const Machine& machine, const std::string& lobbyId, const std::vector<std::string>& members) {
    game_.machine = &machine;
    // The EOS core: the real observables the network subscribes to, and the platform handle EOS_Platform_Tick gets.
    auto* core = static_cast<std::uint8_t*>(game_.New(0x98));
    *reinterpret_cast<void**>(core) = game_.Fn<void*>(kInternalCoreVtable);
    auto* platform = static_cast<void**>(game_.New(8));
    *platform = reinterpret_cast<void*>(0x1000);  // what the fake's EOS_Platform_Create hands out
    *reinterpret_cast<void***>(core + 8) = platform;
    for (std::uintptr_t observable : {0x20, 0x48, 0x70}) game_.Fn<void(*)(void*)>(kObservableCtor)(core + observable);
    core_ = core;

    Result("start-step", "%s", "core");
    users_ = game_.MakeShared(kUsersSize, kUsersRef);
    game_.Fn<void(*)(void*)>(kUsersCtor)(users_.object);

    Result("start-step", "%s", "users");
    manager_ = game_.MakeShared(kManagerSize, kManagerRef);
    const std::function<void(std::shared_ptr<void>)> onReject = [](std::shared_ptr<void>) {};
    auto* filter = ByValue<std::function<bool(const std::string&)>>([](const std::string&) { return true; });
    game_.Fn<void(*)(void*, void*, const void*, void*)>(kManagerCtor)(manager_.object, core_, &onReject, filter);

    Result("start-step", "%s", "manager ctor");
    // Room+0x30 lives as long as the room; so does this.
    LocalUser& local = *new LocalUser{};
    local.id = game_.MakeShared(kEpicIdSize, kEpicIdRef);
    const std::string self = machine.user;
    game_.Fn<void(*)(void*, const char*, const char*)>(kEpicIdCtor)(local.id.object, self.c_str(), "");
    local.name.assign(self.begin(), self.end());
    local.puid = Puid(self);
    const std::string token;
    Shared usersCopy = AddRef(users_);
    game_.Fn<void(*)(void*, Shared*, const char*, LocalUser*, const std::string*)>(kManagerInitialize)(
        manager_.object, &usersCopy, lobbyId.c_str(), &local, &token);

    Result("start-step", "%s", "manager initialize");
    controller_ = game_.MakeShared(kControllerSize, kControllerRef);
    Shared usersForController = AddRef(users_), managerForController = AddRef(manager_);
    game_.Fn<void(*)(void*, void*, Shared*, Shared*, const char*)>(kControllerCtor)(
        controller_.object, core_, &usersForController, &managerForController, lobbyId.c_str());

    Result("start-step", "%s", "controller ctor");
    for (const std::string& member : members) {
        UserDesc desc{};
        desc.puid = Puid(member);
        desc.remote = member != self;
        game_.Fn<void(*)(void*)>(kConnectInfoCtor)(&desc.info);
        if (!desc.remote) {
            desc.info.id = AddRef(local.id);
            desc.info.name = local.name;
        }
        Shared user;
        game_.Fn<void(*)(void*, Shared*, UserDesc*)>(kUsersAdd)(users_.object, &user, &desc);
        if (!user.object) return false;
        members_.emplace_back(member, user);
        Result("start-step", "added %s", member.c_str());
    }
    return true;
}

void Transport::Tick() const { game_.Fn<void(*)(void*)>(kCoreUpdate)(core_); }

void* Transport::User(const std::string& member) const {
    for (const auto& [id, user] : members_)
        if (id == member) return user.object;
    return nullptr;
}

Shared Transport::UserShared(const std::string& member) const {
    for (const auto& [id, user] : members_)
        if (id == member) return user;
    return {};
}

bool Transport::Connected(const std::string& member) const {
    const auto* user = static_cast<const std::uint8_t*>(User(member));
    return user && (*reinterpret_cast<const std::uint32_t*>(user + kUserFlags) & 1) != 0;
}

int Transport::NetworkIndex(const std::string& member) const {
    const auto* user = static_cast<const std::uint8_t*>(User(member));
    return user ? *reinterpret_cast<const std::int32_t*>(user + kUserNetworkIndex) : -1;
}

bool Transport::SendReliable(const std::string& member, std::uint32_t type, const void* data, std::size_t size) const {
    const int index = NetworkIndex(member);
    if (index < 0) return false;
    const std::vector<IndexKey> to{{index, 0}};
    using Send = bool (*)(void*, const std::vector<IndexKey>*, std::uint32_t, const void*, std::size_t, int);
    return game_.Fn<Send>(kSendReliable)(controller_.object, &to, type, data, size, 6);
}

void Transport::Subscribe(std::uint32_t type, Handler handler) {
    using Filter = std::function<bool(const void*)>;
    using Receive = std::function<void(const IndexKey&, const std::int32_t&, const void*, const void*, std::uint64_t)>;
    auto* filter = ByValue<Filter>([type](const void* packet) {
        return (*static_cast<const std::uint32_t*>(packet) & 0xFFFFF) == type;
    });
    auto* receive = ByValue<Receive>(
        [handler](const IndexKey& from, const std::int32_t&, const void*, const void* data, std::uint64_t size) {
            handler(from.index, static_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size));
        });
    Shared token;
    game_.Fn<void(*)(void*, Shared*, Filter*, Receive*)>(kSubscribe)(
        static_cast<std::uint8_t*>(controller_.object) + kDataObservable, &token, filter, receive);
    subscriptions_.push_back(token);
}

}  // namespace gamenet
