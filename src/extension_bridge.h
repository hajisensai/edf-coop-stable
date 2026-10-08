#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>
namespace dn {
class DirectNet;
void bindExtensionTransport(std::shared_ptr<DirectNet> net);
void invalidateExtensionTransport();
void shutdownExtensionTransport() noexcept; // loader-lock safe: atomics only
bool readMissionParticipants(uint64_t& epoch, std::vector<std::string>& members);
void invalidateExtensionParticipant(const std::string& peer);
void setExtensionMissionGateReady(bool ready) noexcept;
}
