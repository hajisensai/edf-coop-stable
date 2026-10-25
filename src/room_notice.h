#pragma once
#include <atomic>
namespace dn {
// What a refused join tells the player. The direct-link part refuses some joins itself (a room Coop persisted - REJOIN,
// its virtual room, the full-lobby fallback - that this machine's All Forces cannot take, mod_room_compat.h) and
// completes the game's JoinLobby with an error; the game says nothing useful about that. The room part shows the line
// in the menu's lower left, outside rooms, where the updater's status goes too (multislot hostmode.cpp, RoomNotice in
// updatecheck.h). One short ASCII line; the log has the details.
using RoomNoticeSink=void(*)(const char* text);
inline std::atomic<RoomNoticeSink>& roomNoticeSinkSlot() {
    static std::atomic<RoomNoticeSink> sink{nullptr};
    return sink;
}
// Set once at load by the room part; nullptr: nobody shows notices (the direct-link part's own tests).
inline void roomNoticeToSink(RoomNoticeSink sink) { roomNoticeSinkSlot().store(sink); }
// From any thread; the sink copies the text. Nothing for nullptr or "".
inline void tellRoomRefused(const char* text) {
    if(!text || !text[0])return;
    if(const RoomNoticeSink sink=roomNoticeSinkSlot().load())sink(text);
}
}
