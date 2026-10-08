#pragma once
#include <memory>
#include <string>
namespace dn {
class DirectNet;
class LobbyMarker;
class RoomView;
// EOS tick only; caller holds the RoomView lock. This is the production publisher
// used by eos_hooks and exercised with the real marker/AF ABI/UDP in its test.
void publishExtensionRoom(std::shared_ptr<DirectNet> net, LobbyMarker& marker,
                          const RoomView& view, const std::string& room, bool parked);
}
