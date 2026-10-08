// Test-only AF presence/readiness exports, isolated from the real game/plugin.
static bool ready = true;
static unsigned version = 1;
extern "C" __declspec(dllexport) void EDF6AF_DecodeRoomType() {}
extern "C" __declspec(dllexport) bool EDF6AF_RoomIsolationReady() { return ready; }
extern "C" __declspec(dllexport) void FakeAF_SetReady(bool value) { ready = value; }
extern "C" __declspec(dllexport) unsigned EDF6AF_SupportProtocolVersion() { return version; }
extern "C" __declspec(dllexport) void FakeAF_SetVersion(unsigned value) { version = value; }
