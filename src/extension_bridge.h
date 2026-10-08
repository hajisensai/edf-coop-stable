#pragma once
#include <memory>
namespace dn {
class DirectNet;
void bindExtensionTransport(std::shared_ptr<DirectNet> net);
void invalidateExtensionTransport();
void shutdownExtensionTransport() noexcept; // loader-lock safe: atomics only
}
