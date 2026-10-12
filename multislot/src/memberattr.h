#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "syncmarker.h"

namespace multislot {

// Room member attributes for other plugins (member_attribute_api.h): keys under EDF6X_, values carried as EOS member
// texts by syncmarker. On the wire a value is "=" + value and a withdrawn one is "-": EOS refuses an empty text (and
// with it our whole member update), and our copy of the lobby keeps a text once seen until it changes, so a removed
// attribute would never read as gone.
namespace memberattr {

constexpr char kKeyPrefix[] = "EDF6X_";
constexpr std::size_t kMaxKeyLength = 32;
constexpr std::size_t kMaxValueLength = 999;  // syncmarker's 1000 less the "=" in front
constexpr std::size_t kMaxKeys = 16;
constexpr char kPresent = '=';
constexpr char kWithdrawn[] = "-";

bool ValidKey(const char* key);
bool ValidValue(const char* value);
// The value a member's wire text stands for; nothing when withdrawn or not ours.
std::optional<std::string> Decode(const std::string& wire);

// Where values go and where keys are asked for: syncmarker in the plugin, a recorder in tests.
struct Sinks {
    std::function<void(const std::string& key, const std::string& wire)> publish;
    std::function<void(const std::string& key)> watch;
};

struct MemberValue {
    std::string id;
    std::uint32_t ordinal = 0;
    bool host = false;
    bool self = false;
    std::optional<std::string> value;
};

struct RoomFacts {
    bool inRoom = false;
    bool host = false;
    std::uint32_t members = 0;
    std::uint64_t generation = 1;
    std::string self;
    std::string owner;
};

// Thread-safe. Observe runs on the EOS tick, the rest on any thread.
class Store {
public:
    // The lobby glue runs: publishes go out. Without it publish refuses.
    void Attach(Sinks sinks);
    bool Publish(const char* key, const char* value);
    bool Clear(const char* key);
    void Observe(const LobbyView& view);
    RoomFacts Room() const;
    // False for an invalid key or the key limit. Copies out; the caller hands them on outside the lock.
    bool Read(const char* key, std::vector<MemberValue>& members, std::uint64_t& generation);

private:
    struct Member {
        std::string id;
        std::uint32_t ordinal;
        std::map<std::string, std::string, std::less<>> wire;  // extension keys only
    };
    bool AdmitKeyLocked(const std::string& key);
    bool SendLocked(const std::string& key, const std::string& wire);
    std::optional<std::string> ValueLocked(const Member& member, const std::string& key) const;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    Sinks sinks_;
    bool attached_ = false;
    std::vector<std::string> keys_;  // admitted: published or read
    std::map<std::string, std::string, std::less<>> own_;  // our wire text per key
    std::string lobby_, self_, owner_;
    std::vector<Member> members_;  // ordinal order
    std::uint32_t nextOrdinal_ = 0;
    std::uint64_t generation_ = 1;
};

Store& MemberAttributes();

// Hooks the store to syncmarker's beat. `lobbyGlue`: InstallSyncMarker worked (otherwise publish refuses).
void StartMemberAttributes(bool lobbyGlue);

}  // namespace memberattr
}  // namespace multislot
