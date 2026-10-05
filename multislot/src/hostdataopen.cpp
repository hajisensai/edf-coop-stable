#include "hostdataopen.h"

#include <atomic>
#include <mutex>
#include <set>
#include <string>

#include "log.h"

namespace multislot {
namespace {

struct OpenState {
    std::atomic<std::shared_ptr<const hostdata::Overlay>> overlay;
    std::mutex lock;
    // Every path the game was pointed at. The game reads the characters after the hook returns, so they are kept
    // for good: at most kMaxFiles per bundle a player ever accepted.
    std::set<std::wstring> paths;
    std::atomic<unsigned> opens{0};
};

OpenState& State() {
    static OpenState state;
    return state;
}

const wchar_t* Keep(std::wstring path) {
    OpenState& state = State();
    std::scoped_lock lock(state.lock);
    return state.paths.insert(std::move(path)).first->c_str();
}

}  // namespace

void HostDataOpenHandler(CpuContext* context) {
    const auto* path = reinterpret_cast<const wchar_t*>(context->rdi);
    const std::string data = hostdata::OpenedDataPath(path);
    if (data.empty()) return;
    OpenState& state = State();
    const std::shared_ptr<const hostdata::Overlay> overlay = state.overlay.load();
    const auto to = overlay ? hostdata::OverlayPath(path, *overlay) : std::nullopt;
    // The probe: which of these files the game reads, when, and from where. Whether it reads them again for
    // each mission decides when the overlay may switch.
    const char* from = path[0] == L'.' ? "mods" : "game";
    Log("Host data: open #%u %s (%s%s)", ++state.opens, data.c_str(), to ? "replaced, not from " : "", from);
    if (to) context->rdi = reinterpret_cast<std::uint64_t>(Keep(*to));
}

void SetHostOverlay(std::shared_ptr<const hostdata::Overlay> overlay) { State().overlay.store(std::move(overlay)); }

std::shared_ptr<const hostdata::Overlay> HostOverlay() { return State().overlay.load(); }

}  // namespace multislot
