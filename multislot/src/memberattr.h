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
// texts by syncmarker (PublishExtensionText: an update of their own, never with Coop's markers). On the wire a value
// is "=" + value and a withdrawn one is "-": EOS refuses an empty text (and with it the whole update), and our copy of
// the lobby keeps a text once seen until it changes, so a removed attribute would never read as gone.
namespace memberattr {

constexpr char kKeyPrefix[] = "EDF6X_";
constexpr std::size_t kMaxKeyLength = 32;
constexpr std::size_t kMaxValueLength = 999;  // syncmarker's 1000 less the "=" in front
constexpr std::size_t kMaxPublishedKeys = 16;
constexpr std::size_t kMaxWatchedKeys = 16;
// All our current values together (every plugin's): what one extension update carries stays far below anything EOS
// could refuse for size, and one plugin cannot crowd the others out of the lobby.
constexpr std::size_t kMaxTotalValueBytes = 4096;
constexpr char kPresent = '=';
constexpr char kWithdrawn[] = "-";

// The API's lastError codes (EDF6COOP_MA_*).
enum class Error : std::uint32_t {
    kNone = 0,
    kInvalidKey = 1,
    kInvalidValue = 2,
    kPublishKeyLimit = 3,
    kWatchKeyLimit = 4,
    kValueBudget = 5,
    kBadArgument = 6,
    kInternal = 7,
};

bool ValidKey(const char* key);
bool ValidValue(const char* value);
// The value a member's wire text stands for; nothing when withdrawn, not ours, or not a value the contract allows
// (printable ASCII, at most kMaxValueLength): a member's text is unauthenticated lobby data.
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

// Thread-safe. Observe runs on the EOS tick, the rest on any thread. Sinks are called under the store's lock (so
// publishes reach syncmarker in call order); syncmarker never calls back into the store under its own lock.
class Store {
public:
    // The lobby glue runs: every key read so far is watched and every value accepted so far goes out.
    void Attach(Sinks sinks);
    // Accepted before Attach too: kept, and sent on Attach.
    Error Publish(const char* key, const char* value);
    Error Clear(const char* key);
    void Observe(const LobbyView& view);
    RoomFacts Room() const;
    // Copies the members' values of `key` out; the caller hands them on outside the lock.
    Error Read(const char* key, std::vector<MemberValue>& members, std::uint64_t& generation);

private:
    struct Member {
        std::string id;
        std::uint32_t ordinal;
        std::map<std::string, std::string, std::less<>> wire;  // extension keys only
    };
    void SendLocked(const std::string& key, std::string wire);
    std::size_t ValueBytesLocked(const std::string& except) const;
    std::optional<std::string> ValueLocked(const Member& member, const std::string& key) const;

    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    Sinks sinks_;
    bool attached_ = false;
    std::vector<std::string> published_;  // keys we publish (kMaxPublishedKeys)
    std::vector<std::string> watched_;    // keys read (kMaxWatchedKeys)
    std::map<std::string, std::string, std::less<>> own_;  // our wire text per key
    std::string lobby_, self_, owner_;
    std::vector<Member> members_;  // ordinal order
    std::uint32_t nextOrdinal_ = 0;
    std::uint64_t generation_ = 1;
};

Store& MemberAttributes();

// Hooks the store to syncmarker's beat. `lobbyGlue`: InstallSyncMarker worked (otherwise values are kept, never sent).
void StartMemberAttributes(bool lobbyGlue);

}  // namespace memberattr
}  // namespace multislot
