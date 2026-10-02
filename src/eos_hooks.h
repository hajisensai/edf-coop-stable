#pragma once
#include <windows.h>

#include <string>

#include "config.h"
#include "direct_net.h"
#include "eos_min.h"

namespace dn {

// Hooks EDF.dll's EOS P2P imports. `net` may be nullptr (diagnostics / EOS tuning only).
bool installEosHooks(HMODULE game, HMODULE eos, const Config& config, DirectNet* net);

// The address this host tells joining players to connect to (published in the lobby). Thread-safe.
void setAdvertisedAddress(const std::string& address);

// Coming back into the room we were last in while Epic's lobby cannot list it (fake_lobby.h): the room
// list gets an entry for it, and joining that goes over the direct link to its host. Installed in front of
// every other wrapper of EDF.dll's lobby imports (after the room part's), so a LobbyDetails handle of ours
// never reaches one of them or EOS. Needs installEosHooks with the direct link.
bool installVirtualRoomHooks(HMODULE game);

// EOS_LobbyDetails_CopyInfo / EOS_LobbyDetails_Info_Release for code outside EDF.dll's imports that is
// handed the game's LobbyDetails handles: these also answer ours. Only once installVirtualRoomHooks succeeded.
EOS_EResult lobbyDetailsCopyInfo(EOS_HLobbyDetails details, const EOS_LobbyDetails_CopyInfoOptions* options,
                                 EOS_LobbyDetails_Info** out);
void lobbyDetailsInfoRelease(EOS_LobbyDetails_Info* info);

// Whether `remote` (an EOS_ProductUserId) plays with us over a direct link now. A direct link is only made
// between builds of the same protocol, all of which read a split mission start message.
bool directMemberReadsSplitSync(const void* remote);

// Process is exiting: from now on every hook forwards straight to EOS (no locks, no logging).
void eosHooksShutdown();

}  // namespace dn
