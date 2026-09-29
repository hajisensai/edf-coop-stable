#pragma once
#include <cstddef>
#include <cstdint>

namespace multislot {

// Who the log belongs to, and who each member is.
//
// The EOS SDK writes every peer as an abbreviation of its ProductUserId - "[000...044]" - which says
// nothing about the person. When five friends compare their logs after a session, the first question is
// always "which of these is me and which one is 044", and until now no line in the file answered it.
//
// So: this machine writes its Steam name and SteamID64 once (WHOAMI), every room user slot carries the
// full ProductUserId of whoever took it, and the room screen writes the roster with the names the
// members are actually playing under. A name ties a log to a person, a ProductUserId ties a line in one
// log to a line in another, and the SteamID ties both to the friend list.
//
// Nothing here touches the game. Steam and EOS are asked through their own public exports, both calls
// are read-only, and everything runs on the game's UI thread inside __try: if either SDK is not there,
// or answers with nothing, the mod logs that it could not read it and carries on.

// Called every menu frame (hostmode.cpp). Writes the WHOAMI line once, as soon as Steam answers; until
// then it retries, because the plugin loads before the game signs in. Cheap after it has succeeded.
void PollIdentity();

// The EOS ProductUserId behind `id` as its 32 characters, written into `out` (33 bytes is enough).
// Returns `out`, which holds "" when the id is null, invalid, or the SDK is not loaded.
const char* ProductUserIdText(const void* id, char* out, std::size_t size);

// A std::wstring the game owns, as UTF-8 in `out`. Empty when it does not read as a string. The log is
// written byte for byte, so a Japanese name survives; %ls in the C locale would not. The name is whatever
// the member typed, so control characters and line separators come out as '?': a name can neither end a log
// line nor start a new one that looks like the plugin's own.
bool NameText(const void* wstring, char* out, std::size_t size);
// Anything longer than this is not a player name and the read is refused rather than trusted.
constexpr std::size_t kMaxNameChars = 64;
// `out` bytes that hold any accepted name: UTF-8 takes at most three bytes per UTF-16 unit (a surrogate pair is
// four bytes for two units), plus the terminator. 1.5.13 used 128, and a long Japanese name came out empty.
constexpr std::size_t kNameTextBytes = kMaxNameChars * 3 + 1;

}  // namespace multislot
