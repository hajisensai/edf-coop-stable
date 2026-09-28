#pragma once
#include <windows.h>

namespace dn {

// Address of the IAT slot through which `module` calls `dll!function`, or nullptr.
void** findImportSlot(HMODULE module, const char* dll, const char* function);

// Replaces the slot with `hook`; the previous target is stored in `*original`.
bool patchImport(HMODULE module, const char* dll, const char* function, void* hook, void** original);

}  // namespace dn
