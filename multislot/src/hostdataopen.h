// Host data in the game (hostdata.h): the file open hook that points the game at the host's files, and the
// probe that logs when weapon and vehicle files are read.
#pragma once

#include <memory>

#include "hostdata.h"
#include "midhook.h"

namespace multislot {

// HostDataHooks (patches.h). On any thread the game opens files on.
void HostDataOpenHandler(CpuContext* context);

// The host's files the game reads from now on (nullptr: the player's own). Files the game already holds are
// not read again; that is why the menu switches it only between missions (hostdatanet.h). Any thread.
void SetHostOverlay(std::shared_ptr<const hostdata::Overlay> overlay);
std::shared_ptr<const hostdata::Overlay> HostOverlay();

}  // namespace multislot
