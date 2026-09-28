#pragma once
#include <windows.h>

#include "config.h"
#include "direct_net.h"

namespace dn {

// Hooks EDF.dll's EOS P2P imports. `net` may be nullptr (diagnostics / EOS tuning only).
bool installEosHooks(HMODULE game, HMODULE eos, const Config& config, DirectNet* net);

// Process is exiting: from now on every hook forwards straight to EOS (no locks, no logging).
void eosHooksShutdown();

}  // namespace dn
