#include "direct_net.h"
#include "extension_room.h"
#include "extension_bridge.h"
#include "lobby_marker.h"
#include "room_view.h"
#include <algorithm>

namespace dn {
void publishExtensionRoom(std::shared_ptr<DirectNet> net, LobbyMarker& marker,
                          const RoomView& view, const std::string& room, bool parked) {
    uint64_t worldEpoch = 0;
    std::vector<std::string> participants;
    const bool known = readMissionParticipants(worldEpoch, participants);
    const auto owner = marker.ownerId();
    auto gameMembers = view.members();
    std::sort(gameMembers.begin(), gameMembers.end());
    const bool valid = net && known && !owner.empty() && marker.extensionCompatible(participants) &&
        !parked && view.active() && !view.awaitingHost() && !room.empty() &&
        std::includes(gameMembers.begin(), gameMembers.end(), participants.begin(), participants.end());
    if (!valid) { invalidateExtensionTransport(); return; }
    net->setExtensionRoom(room, owner, std::move(participants), worldEpoch);
    bindExtensionTransport(std::move(net));
}
}
