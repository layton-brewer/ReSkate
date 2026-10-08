#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Hall Of Meat's view of the local skater's skeleton, for the X-ray. The game thread reads the
// animation pose every client tick (hom_rig.cpp) and publishes one world-space segment per bone of
// the original's X-ray skeleton (the 19 `dem_bones_hom` meshes); the HUD projects them.
namespace dingosdk::hall_of_meat {
// Same order as bones() in hom_core.cpp.
inline constexpr std::size_t rig_bones = 19;
struct RigSegment {
    std::array<float, 3> a{}, b{};
    float radius{}; // metres
};
struct RigView {
    bool valid{};
    std::array<RigSegment, rig_bones> bones{};
    std::array<float, 3> forward{0, 0, 1}; // the skater's body forward (pelvis local +Y)
    std::uintptr_t base{};
    std::chrono::steady_clock::time_point at{};
};
namespace rig_detail {
inline std::mutex mutex;
inline RigView latest;
} // namespace rig_detail
inline void publish_rig(const RigView &view) noexcept {
    std::lock_guard lock(rig_detail::mutex);
    rig_detail::latest = view;
}
// Nothing when the skeleton was not read recently.
inline RigView latest_rig(std::chrono::milliseconds max_age = std::chrono::milliseconds(250)) noexcept {
    std::lock_guard lock(rig_detail::mutex);
    if (!rig_detail::latest.valid || std::chrono::steady_clock::now() - rig_detail::latest.at > max_age) return {};
    return rig_detail::latest;
}
// Set by the HUD while a bail is on screen: the game's own interface is hidden meanwhile.
inline std::atomic<bool> &hide_game_ui() {
    static std::atomic<bool> value{};
    return value;
}
// Whether the skater stands upright (head well above the hips), from the last skeleton read.
inline std::atomic<bool> &rig_upright() {
    static std::atomic<bool> value{};
    return value;
}
inline std::atomic<bool> &rig_probe_requested() {
    static std::atomic<bool> value{};
    return value;
}
// Game thread, once per client tick, before the bail tracker.
void rig_tick(std::uintptr_t base, std::uintptr_t client) noexcept;
} // namespace dingosdk::hall_of_meat
