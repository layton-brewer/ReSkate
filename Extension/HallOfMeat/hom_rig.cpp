#include "hom_rig.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Multiplayer/Remote/native_pose_layout.h"
#include "Extension/Skater/client_source_spawn_internal.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include <Windows.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace dingosdk::hall_of_meat {
namespace {
using client_source::detail::first_person_read;
void say(const std::string &text) { logging::log(logging::Level::info, logging::Channel::assets, "HoM rig: {}", text); }

// A table of `count` parent indices (signed 16 or 32 bit): parent[0] is -1 or 0 and every
// other entry names an earlier joint.
bool looks_like_parents(const std::vector<std::int32_t> &p) {
    if (p.empty() || (p[0] != -1 && p[0] != 0 && p[0] != 0xFFFF)) return false;
    std::size_t earlier = 0;
    for (std::size_t i = 1; i < p.size(); ++i)
        if (p[i] >= 0 && static_cast<std::size_t>(p[i]) < i) ++earlier;
    return earlier + 4 >= p.size() - 1;
}

void probe(std::uintptr_t base, std::uintptr_t client) {
    const auto component = client_source::detail::first_person_component(base, client);
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder) return say("no animation holder");
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    if (!pose.buffer) return say("no pose buffer");
    say(std::format("{} joints, buffer {:#x}", pose.count, pose.buffer));

    // The pose, parent-local: scale.xyz, rotation.xyzw, position.xyz per joint.
    for (std::uint32_t first = 0; first < pose.count; first += 6) {
        std::string line;
        for (std::uint32_t i = first; i < std::min<std::uint32_t>(first + 6, pose.count); ++i) {
            std::array<float, 12> b{};
            if (!first_person_read(pose.buffer + i * 0x30ULL, b.data(), sizeof(b))) continue;
            line += std::format("[{}] p({:.3f},{:.3f},{:.3f}) s{:.2f}  ", i, b[8], b[9], b[10], b[0]);
        }
        say(line);
    }

    // Look for the skeleton's parent table among the resource's pointers.
    std::uintptr_t rig{}, definition{}, resource{};
    if (!first_person_read(holder + 0x78, &rig, 8) || !rig || !first_person_read(rig + 0x18, &definition, 8) || !definition ||
        !first_person_read(definition + 0x1a0, &resource, 8) || !resource)
        return say("no skeleton resource");
    say(std::format("resource {:#x}", resource));
    std::array<std::uint64_t, 32> head{};
    first_person_read(resource, head.data(), sizeof(head));
    std::string words;
    for (std::size_t i = 0; i < head.size(); ++i) words += std::format("{:#x} ", head[i]);
    say("resource head: " + words);
    for (std::size_t slot = 0; slot < head.size(); ++slot) {
        const auto pointer = static_cast<std::uintptr_t>(head[slot]);
        if (pointer < 0x10000 || pointer > 0x00007fffffffffffULL) continue;
        for (const std::size_t width : {2u, 4u}) {
            std::vector<unsigned char> raw(pose.count * width);
            if (!first_person_read(pointer, raw.data(), raw.size())) continue;
            std::vector<std::int32_t> values(pose.count);
            for (std::uint32_t i = 0; i < pose.count; ++i) {
                if (width == 2) {
                    std::int16_t v{};
                    std::memcpy(&v, raw.data() + i * 2, 2);
                    values[i] = v;
                } else {
                    std::memcpy(&values[i], raw.data() + i * 4, 4);
                }
            }
            if (!looks_like_parents(values)) continue;
            say(std::format("parent table candidate: resource+{:#x} -> {:#x}, {}-bit", slot * 8, pointer, width * 8));
            for (std::uint32_t first = 0; first < pose.count; first += 24) {
                std::string line;
                for (std::uint32_t i = first; i < std::min<std::uint32_t>(first + 24, pose.count); ++i) line += std::format("{} ", values[i]);
                say(std::format("parents[{}..]: {}", first, line));
            }
        }
    }
    say("done");
}

// The pose is parent-local {scale, Hamilton quaternion, translation} per joint; joint 1 carries
// the world placement, so composing from the root lands in world space (client_first_person.cpp).
// The 395-joint AnimBase skeleton, mapped from a live dump (hom rig, 2026-10-08): the spine runs
// 7 (pelvis) 42 43 44 45 (chest) 101 102 (neck) 103 (head) along local +X; legs hang off the
// pelvis (8-11 and 341-344: hip, knee, ankle, ball) and arms off the chest (46-49 and 275-278:
// clavicle, shoulder, elbow, wrist). Local -Z is the skater's right, so 8.. and 46.. are right.
struct Joint {
    std::array<float, 4> q{0, 0, 0, 1};
    std::array<float, 3> p{};
    float s = 1;
};
std::array<float, 3> rotate(const std::array<float, 4> &q, const std::array<float, 3> &v) {
    const float tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    return {v[0] + q[3] * tx + (q[1] * tz - q[2] * ty), v[1] + q[3] * ty + (q[2] * tx - q[0] * tz), v[2] + q[3] * tz + (q[0] * ty - q[1] * tx)};
}
bool child(const Joint &parent, std::uintptr_t buffer, std::uint16_t index, Joint &out) {
    std::array<float, 12> bone{};
    if (!first_person_read(buffer + index * 0x30ULL, bone.data(), sizeof(bone))) return false;
    for (const std::size_t i : {0u, 1u, 2u, 4u, 5u, 6u, 7u, 8u, 9u, 10u})
        if (!std::isfinite(bone[i]) || std::abs(bone[i]) > 1000000.0f) return false;
    const auto &q = parent.q;
    const auto offset = rotate(q, {bone[8] * parent.s, bone[9] * parent.s, bone[10] * parent.s});
    for (std::size_t i = 0; i < 3; ++i) out.p[i] = parent.p[i] + offset[i];
    out.q = {q[3] * bone[4] + q[0] * bone[7] + q[1] * bone[6] - q[2] * bone[5], q[3] * bone[5] - q[0] * bone[6] + q[1] * bone[7] + q[2] * bone[4],
             q[3] * bone[6] + q[0] * bone[5] - q[1] * bone[4] + q[2] * bone[7], q[3] * bone[7] - q[0] * bone[4] - q[1] * bone[5] - q[2] * bone[6]};
    out.s = parent.s * (bone[0] + bone[1] + bone[2]) / 3.0f;
    return std::isfinite(out.s) && out.s > 0.01f;
}
constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 26> chains{{
    {1, 0}, {7, 1}, {42, 7}, {43, 42}, {44, 43}, {45, 44}, {101, 45}, {102, 101}, {103, 102},
    {8, 7}, {9, 8}, {10, 9}, {11, 10}, {341, 7}, {342, 341}, {343, 342}, {344, 343},
    {46, 45}, {47, 46}, {48, 47}, {49, 48}, {275, 45}, {276, 275}, {277, 276}, {278, 277}, {0, 0}}};
std::array<float, 3> along(const std::array<float, 3> &from, const std::array<float, 3> &to, float metres) {
    std::array<float, 3> d{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-5f) return to;
    return {to[0] + d[0] / l * metres, to[1] + d[1] / l * metres, to[2] + d[2] / l * metres};
}
std::array<float, 3> lerp(const std::array<float, 3> &a, const std::array<float, 3> &b, float t) {
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
void publish_view(std::uintptr_t base, std::uintptr_t client) {
    const auto component = client_source::detail::first_person_component(base, client);
    std::uintptr_t holder{};
    if (!first_person_read(component + 0xa0, &holder, 8) || !holder) return;
    const auto pose = multiplayer::read_native_pose_layout(first_person_read, base, holder, 512);
    if (!pose.buffer || pose.count != 395) return;
    std::array<Joint, 395> j{};
    std::array<bool, 395> have{};
    have[0] = true;
    if (!child(Joint{}, pose.buffer, 0, j[0])) return;
    for (std::size_t i = 0; i + 1 < chains.size(); ++i) {
        const auto [index, parent] = chains[i];
        if (!have[parent] || !child(j[parent], pose.buffer, index, j[index])) return;
        have[index] = true;
    }
    const auto P = [&](std::uint16_t i) { return j[i].p; };
    RigView view;
    const auto head_top = [&] {
        const auto up = rotate(j[103].q, {0.17f * j[103].s, 0, 0});
        return std::array<float, 3>{j[103].p[0] + up[0], j[103].p[1] + up[1], j[103].p[2] + up[2]};
    }();
    // Order: Skull, Neck, Rib Cage, Lower Spine, Hips, Bicep L/R, Forearm L/R, Hand L/R, Thigh L/R,
    // Calf L/R, Ankle L/R, Toes L/R (left first, as in bones()).
    view.bones = {{
        {P(103), head_top, 0.085f},
        {P(101), P(103), 0.035f},
        {P(44), P(101), 0.12f},
        {P(7), P(43), 0.05f},
        {P(341), P(8), 0.06f},
        {P(276), P(277), 0.035f}, {P(47), P(48), 0.035f},
        {P(277), P(278), 0.03f}, {P(48), P(49), 0.03f},
        {P(278), along(P(277), P(278), 0.09f), 0.03f}, {P(49), along(P(48), P(49), 0.09f), 0.03f},
        {P(341), P(342), 0.05f}, {P(8), P(9), 0.05f},
        {P(342), P(343), 0.04f}, {P(9), P(10), 0.04f},
        {P(343), lerp(P(343), P(344), 0.5f), 0.035f}, {P(10), lerp(P(10), P(11), 0.5f), 0.035f},
        {P(344), along(P(343), P(344), 0.07f), 0.025f}, {P(11), along(P(10), P(11), 0.07f), 0.025f},
    }};
    view.valid = true;
    // Upright: the head at least half a metre above the hips (standing or walking off).
    rig_upright().store(P(103)[1] - P(7)[1] > 0.5f);
    view.base = base;
    view.at = std::chrono::steady_clock::now();
    publish_rig(view);
}

// The game's UI draw flag (what ReSkate's own `hideui` toggles): cleared while a bail is on
// screen, put back afterwards unless someone else changed it meanwhile.
struct UiHide {
    bool applied{};
    std::uintptr_t object{};
};
bool write_byte(std::uintptr_t address, std::uint8_t value) noexcept {
    __try {
        *reinterpret_cast<volatile std::uint8_t *>(address) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void apply_ui_hide(std::uintptr_t base) noexcept {
    static UiHide hide;
    const bool want = hide_game_ui().load();
    namespace spawn = addr::client_source_spawn;
    std::uintptr_t object{}, vtable{}, type{};
    if (!first_person_read(base + spawn::ui_settings, &object, 8) || !object || !first_person_read(object, &vtable, 8) ||
        vtable != base + spawn::ui_settings_vtable || !first_person_read(object + 8, &type, 8) || type != base + spawn::ui_settings_type)
        return;
    std::uint8_t draw{};
    if (!first_person_read(object + 0x4e, &draw, 1) || draw > 1) return;
    if (want && !hide.applied && draw == 1) {
        if (write_byte(object + 0x4e, 0)) hide = {true, object};
    } else if (!want && hide.applied) {
        if (hide.object == object && draw == 0) (void)write_byte(object + 0x4e, 1);
        hide = {};
    }
}
} // namespace

void rig_tick(std::uintptr_t base, std::uintptr_t client) noexcept {
    try {
        publish_view(base, client);
    } catch (...) {
    }
    apply_ui_hide(base);
    if (!rig_probe_requested().exchange(false)) return;
    try {
        probe(base, client);
    } catch (const std::exception &error) {
        say(std::string("failed: ") + error.what());
    } catch (...) {
        say("failed");
    }
}
} // namespace dingosdk::hall_of_meat
