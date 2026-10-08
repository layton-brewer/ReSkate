#include "hall_of_meat_hud.h"
#include "hom_art.h"
#include "hom_bones.h"
#include "hom_core.h"
#include "hom_rig.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/engine.h"
#include "Engine/Game/Build/20260929/client_source_spawn.h"
#include "Engine/Game/UI/game_view.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Trainer/trainer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <imgui.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Hall Of Meat HUD, laid out from the original (Skate 3): bottom left, a bone chip and a stack of
// metric rows (rotation, air time, drop, bail time) over the Thrasher "Hall of Meat" mark and the
// score; a dark, tinted vignette while the skater is a ragdoll.
namespace dingosdk::overlay {
namespace {
using namespace dingosdk::hall_of_meat;
using Clock = std::chrono::steady_clock;

constexpr double linger_seconds = 4.0, fade_seconds = 0.8;
constexpr ImU32 cyan = IM_COL32(150, 232, 240, 255);
constexpr ImU32 white = IM_COL32(246, 246, 246, 255);
constexpr ImU32 chip_blue = IM_COL32(32, 168, 214, 235);

// A timed challenge in the style of the original's: a title, a few objectives and a score goal.
struct Objective {
    std::string text;
    int kind{}; // 0: one bail of at least `target` points, 1: `target` bones in one bail,
                // 2: break a bone of `region`/`word`, 3: `target` bones in total
    int target{};
    std::string region, word;
    bool done{};
};
struct Challenge {
    std::string title;
    int minutes{5};
    int goal{};
    std::vector<Objective> objectives;
};
const std::vector<Challenge> &challenges() {
    // Names and objectives are the ones seen in the original game.
    static const std::vector<Challenge> list{
        {"Bone Kubes", 5, 60000,
         {{"25,000pt Bail", 0, 25000}, {"Break a Leg Bone", 2, 0, "Leg"}, {"Break your Collar Bone", 2, 0, "", "collarbone"},
          {"Break 6 Bones in One Bail", 1, 6}}},
        {"Thorax Crunch", 5, 1000000, {{"Break 50 bones", 3, 50}, {"Do a 90,000 point bail", 0, 90000}}},
        {"Meat Chute", 5, 200000, {{"Break 10 bones", 3, 10}, {"Do a 40,000 point bail", 0, 40000}}},
    };
    return list;
}
std::string lower_case(std::string text) {
    for (auto &ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}
struct Session {
    bool active{}, finished{}, won{};
    Challenge challenge;
    double started{}, ends{}, message_until{};
    std::string message;
    int total{}, bones{};
};

struct State {
    Session session;
    std::mutex mutex; // the game thread feeds the tracker, the presentation thread draws it
    Tracker tracker;
    double visible_until{};
    double started{};
    float shown_total{};
    int best{};
    std::uint32_t last_state{};
    double last_time{-1};
    double heartbeat{};
    float darkness{};
};
State &state() {
    static State value;
    return value;
}

double clock_seconds() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

std::string with_commas(int value) {
    auto text = std::to_string(value);
    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) text.insert(static_cast<std::size_t>(i), ",");
    return text;
}
ImU32 alpha(ImU32 colour, float a) {
    const auto base = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff);
    return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(base * a, 0.0f, 255.0f)) << IM_COL32_A_SHIFT);
}
void shadowed(ImDrawList *draw, ImFont *font, float size, ImVec2 at, ImU32 colour, const std::string &text) {
    const float offset = std::max(1.0f, size / 14.0f);
    const auto a = static_cast<float>((colour >> IM_COL32_A_SHIFT) & 0xff) / 255.0f;
    draw->AddText(font, size, ImVec2(at.x + offset, at.y + offset), alpha(IM_COL32(0, 0, 0, 255), a * 0.85f), text.c_str());
    draw->AddText(font, size, at, colour, text.c_str());
}
void image(ImDrawList *draw, std::string_view name, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    HomArt art;
    if (hom_art(name, art)) draw->AddImage(art.texture, a, b, art.uv0, art.uv1, tint);
}
bool has_art(std::string_view name) {
    HomArt art;
    return hom_art(name, art);
}

// The dark, bluish fisheye the original draws over the world while the skater is a ragdoll.
void draw_vignette(ImDrawList *draw, ImVec2 size, float strength) {
    if (strength <= 0.0f) return;
    const ImU32 edge = IM_COL32(8, 10, 28, static_cast<int>(210 * strength)), none = IM_COL32(8, 10, 28, 0);
    const float band_x = size.x * 0.22f, band_y = size.y * 0.30f;
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(band_x, size.y), edge, none, none, edge);
    draw->AddRectFilledMultiColor(ImVec2(size.x - band_x, 0), ImVec2(size.x, size.y), none, edge, edge, none);
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, band_y), edge, edge, none, none);
    draw->AddRectFilledMultiColor(ImVec2(0, size.y - band_y), ImVec2(size.x, size.y), none, none, edge, edge);
}

// Small icons for rows the original art does not give us a texture for.
void icon_rotation(ImDrawList *draw, ImVec2 c, float r, ImU32 colour) {
    draw->PathArcTo(c, r * 0.75f, -0.3f, 4.6f, 24);
    draw->PathStroke(colour, 0, r * 0.28f);
    draw->AddTriangleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.5f), ImVec2(c.x + r * 0.35f, c.y - r * 0.55f),
                            ImVec2(c.x + r * 0.7f, c.y + r * 0.05f), colour);
}
void icon_air(ImDrawList *draw, ImVec2 c, float r, ImU32 colour) {
    draw->AddTriangleFilled(ImVec2(c.x, c.y - r), ImVec2(c.x - r * 0.9f, c.y + r * 0.6f), ImVec2(c.x + r * 0.9f, c.y + r * 0.6f), colour);
    draw->AddTriangleFilled(ImVec2(c.x, c.y + r * 0.25f), ImVec2(c.x - r * 0.35f, c.y + r), ImVec2(c.x + r * 0.35f, c.y + r), alpha(colour, 0.7f));
}

struct Row {
    std::string_view icon;
    int kind;  // 0 rotation, 1 air, 2 drop, 3 duration
    std::string value, points;
};

void draw_block(ImDrawList *draw, const Result &r, float total_shown, float fade, float scale, double now, double started) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto *font = hom_font(28) ? hom_font(28) : ImGui::GetFont();
    auto *score_font = hom_font(60) ? hom_font(60) : font;
    auto *small_font = hom_font(22) ? hom_font(22) : font;
    const float left = display.x * 0.083f, right = display.x * 0.23f, bottom = display.y * 0.925f;
    const float width = right - left;
    const float intro = std::clamp(static_cast<float>((now - started) / 0.25), 0.0f, 1.0f);
    const float a = fade * intro;
    const float slide = (1.0f - intro) * -24.0f * scale;
    const float cx = left + width * 0.5f + slide;

    // Score and mark.
    const float score_size = 60.0f * scale;
    const auto score = with_commas(static_cast<int>(total_shown));
    const auto extent = score_font->CalcTextSizeA(score_size, FLT_MAX, 0.0f, score.c_str());
    const float score_y = bottom - score_size - 2.0f * scale;
    shadowed(draw, score_font, score_size, ImVec2(cx - extent.x * 0.5f, score_y), alpha(white, a), score);
    draw->AddRectFilled(ImVec2(left + slide, bottom), ImVec2(right + slide, bottom + 2.0f * scale), alpha(IM_COL32(170, 220, 235, 190), a));

    const float hom_size = 22.0f * scale;
    const auto hom = std::string("Hall of Meat");
    const auto hom_extent = small_font->CalcTextSizeA(hom_size, FLT_MAX, 0.0f, hom.c_str());
    const float hom_y = score_y - hom_size - 2.0f * scale;
    shadowed(draw, small_font, hom_size, ImVec2(cx - hom_extent.x * 0.5f, hom_y), alpha(IM_COL32(236, 236, 236, 255), a), hom);

    const float logo_h = 62.0f * scale;
    const float logo_y = hom_y - logo_h + 2.0f * scale;
    if (has_art("thrasher")) {
        HomArt logo;
        hom_art("thrasher", logo);
        const float logo_w = logo_h * logo.width / std::max(1.0f, logo.height);
        image(draw, "thrasher", ImVec2(cx - logo_w * 0.5f, logo_y), ImVec2(cx + logo_w * 0.5f, logo_y + logo_h), alpha(cyan, a));
    } else {
        const auto name = std::string("THRASHER");
        const float s = logo_h * 0.95f;
        const auto e = font->CalcTextSizeA(s, FLT_MAX, 0.0f, name.c_str());
        shadowed(draw, font, s, ImVec2(cx - e.x * 0.5f, logo_y), alpha(cyan, a), name);
    }

    // Metric rows, newest at the bottom like the original: rotation, air, drop, bail time.
    std::vector<Row> rows;
    if (r.rotation >= 1.0f) rows.push_back({"", 0, std::format("{:.0f}\xC2\xB0", r.rotation), r.rotation_points > 0 ? with_commas(r.rotation_points) : ""});
    if (r.air_time > 0.0f) rows.push_back({"", 1, std::format("{:05.2f}s", r.air_time), with_commas(r.air_points)});
    if (r.drop >= 0.5f) rows.push_back({"arrow", 2, std::format("{:.1f} m", r.drop), with_commas(r.drop_points)});
    if (r.duration > 0.0f) rows.push_back({"timer", 3, std::format("{:05.2f}s", r.duration), with_commas(r.duration_points)});

    const float row_h = 36.0f * scale, text_size = 28.0f * scale;
    float y = logo_y - 12.0f * scale - row_h;
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const ImVec2 icon_centre(left + 22.0f * scale + slide, y + row_h * 0.5f);
        const float ir = 12.0f * scale;
        const ImU32 icon_colour = alpha(cyan, a);
        if (it->kind == 0) icon_rotation(draw, icon_centre, ir, icon_colour);
        else if (it->kind == 1) icon_air(draw, icon_centre, ir, icon_colour);
        else if (has_art(it->icon)) image(draw, it->icon, ImVec2(icon_centre.x - ir, icon_centre.y - ir), ImVec2(icon_centre.x + ir, icon_centre.y + ir), icon_colour);
        const float text_y = y + (row_h - text_size) * 0.5f;
        shadowed(draw, font, text_size, ImVec2(left + 50.0f * scale + slide, text_y), alpha(white, a), it->value);
        if (!it->points.empty()) {
            const auto e = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, it->points.c_str());
            shadowed(draw, font, text_size, ImVec2(right - e.x + slide, text_y), alpha(white, a), it->points);
        }
        y -= row_h;
    }

    // The bone chip on top: icon, count, points.
    if (!r.broken.empty()) {
        const float chip_h = row_h * 0.9f;
        const ImVec2 a0(left + slide, y + (row_h - chip_h) * 0.5f);
        draw->AddRectFilled(a0, ImVec2(left + 112.0f * scale + slide, a0.y + chip_h), alpha(chip_blue, a), 3.0f * scale);
        if (has_art("bones")) image(draw, "bones", ImVec2(a0.x + 4.0f * scale, a0.y + 3.0f * scale), ImVec2(a0.x + chip_h - 3.0f * scale, a0.y + chip_h - 3.0f * scale), alpha(IM_COL32_WHITE, a));
        shadowed(draw, font, 22.0f * scale, ImVec2(a0.x + chip_h + 4.0f * scale, a0.y + 4.0f * scale), alpha(white, a), std::format("x{}", r.broken.size()));
        shadowed(draw, font, text_size, ImVec2(left + 122.0f * scale + slide, a0.y + 2.0f * scale), alpha(white, a), with_commas(r.bone_points));
    }
}

// The camera as it is now (the game moves it after the client tick), as the nametags read it.
bool live_camera(std::uintptr_t base, std::array<float, 16> &world, float &fov) {
    const auto view = latest_game_view();
    if (!view) return false;
    world = view->world;
    fov = view->vertical_fov;
    if (base && view->camera && !(view->camera & 7)) {
        std::uintptr_t vtable{};
        if (memory::peek(view->camera, vtable) &&
            (vtable == base + addr::engine::camera_vtable || vtable == base + addr::client_source_spawn::free_camera_vtable)) {
            std::array<float, 16> live{};
            float live_fov{};
            bool sound = memory::peek(view->camera + 0x50, live) && memory::peek(view->camera + 0xac, live_fov) && std::isfinite(live_fov) &&
                         live_fov > 1 && live_fov < 175;
            for (const auto value : live) sound = sound && std::isfinite(value) && std::abs(value) < 1e7f;
            if (sound) {
                world = live;
                fov = live_fov;
            }
        }
    }
    return fov > 1 && fov < 175;
}

struct Projector {
    std::array<float, 16> m{};
    float focal{};
    ImVec2 centre{};
    // Screen point and pixels per metre at that depth; false behind the camera.
    bool operator()(const std::array<float, 3> &p, ImVec2 &at, float &per_metre) const {
        const float dx = p[0] - m[12], dy = p[1] - m[13], dz = p[2] - m[14];
        const float depth = -(dx * m[8] + dy * m[9] + dz * m[10]);
        if (depth < 0.15f) return false;
        const float side = dx * m[0] + dy * m[1] + dz * m[2], height = dx * m[4] + dy * m[5] + dz * m[6];
        at = ImVec2(centre.x + side * focal / depth, centre.y - height * focal / depth);
        per_metre = focal / depth;
        return true;
    }
};

// One X-ray bone: a pale shaft with knobbed ends, glowing red and pulsing once broken, with a
// white flash and a crack across it at the moment it went.
void draw_bone(ImDrawList *draw, std::size_t index, ImVec2 a, ImVec2 b, float r, bool broken, float since_break, float fade) {
    const ImU32 healthy = IM_COL32(190, 235, 255, 255), hurt = IM_COL32(255, 70, 40, 255), hot = IM_COL32(255, 200, 120, 255);
    const float pulse = broken ? 0.75f + 0.25f * std::sin(static_cast<float>(ImGui::GetTime()) * 9.0f) : 1.0f;
    const ImU32 colour = broken ? hurt : healthy;
    const float base_alpha = (broken ? 0.95f : 0.42f) * fade;
    // Glow.
    for (int layer = 3; layer >= 1; --layer) {
        const float w = r * (1.0f + 0.9f * layer) * (broken ? 1.3f : 1.0f);
        const float a_ = base_alpha * (broken ? 0.16f : 0.07f) * pulse;
        if (index == 0) draw->AddCircleFilled(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), w * 1.2f, alpha(colour, a_), 24);
        else draw->AddLine(a, b, alpha(colour, a_), w * 2.0f);
    }
    if (index == 0) {
        // Skull: a round cranium with dark sockets.
        const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        draw->AddCircleFilled(c, r * 1.15f, alpha(colour, base_alpha), 24);
        draw->AddCircleFilled(ImVec2(c.x - r * 0.4f, c.y + r * 0.15f), r * 0.28f, alpha(IM_COL32(10, 12, 30, 255), base_alpha * 0.8f), 12);
        draw->AddCircleFilled(ImVec2(c.x + r * 0.4f, c.y + r * 0.15f), r * 0.28f, alpha(IM_COL32(10, 12, 30, 255), base_alpha * 0.8f), 12);
    } else {
        const float shaft = index == 2 || index == 4 ? r * 1.6f : r * 1.1f;
        draw->AddLine(a, b, alpha(colour, base_alpha), shaft);
        draw->AddCircleFilled(a, r * 0.95f, alpha(colour, base_alpha), 16);
        draw->AddCircleFilled(b, r * 0.95f, alpha(colour, base_alpha), 16);
        if (index == 2) {
            // Rib cage: ribs across the column.
            const ImVec2 d(b.x - a.x, b.y - a.y);
            const float l = std::max(1.0f, std::sqrt(d.x * d.x + d.y * d.y));
            const ImVec2 n(-d.y / l, d.x / l);
            for (int i = 1; i <= 4; ++i) {
                const float t = i / 5.0f;
                const ImVec2 m(a.x + d.x * t, a.y + d.y * t);
                draw->AddLine(ImVec2(m.x - n.x * r * 2.2f, m.y - n.y * r * 2.2f), ImVec2(m.x + n.x * r * 2.2f, m.y + n.y * r * 2.2f),
                              alpha(colour, base_alpha * 0.85f), std::max(1.5f, r * 0.35f));
            }
        }
    }
    if (broken) {
        // The crack, and the flash when it snapped.
        const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        const ImVec2 d(b.x - a.x, b.y - a.y);
        const float l = std::max(1.0f, std::sqrt(d.x * d.x + d.y * d.y));
        const ImVec2 n(-d.y / l, d.x / l), t(d.x / l, d.y / l);
        const float s = r * 1.6f;
        const ImVec2 zig[]{ImVec2(m.x - n.x * s, m.y - n.y * s), ImVec2(m.x - n.x * s * 0.3f + t.x * s * 0.35f, m.y - n.y * s * 0.3f + t.y * s * 0.35f),
                           ImVec2(m.x + n.x * s * 0.3f - t.x * s * 0.35f, m.y + n.y * s * 0.3f - t.y * s * 0.35f), ImVec2(m.x + n.x * s, m.y + n.y * s)};
        draw->AddPolyline(zig, 4, alpha(IM_COL32(20, 0, 0, 255), 0.9f * fade), 0, std::max(1.5f, r * 0.35f));
        if (since_break < 0.6f) {
            const float k = since_break / 0.6f;
            draw->AddCircleFilled(m, s * (0.8f + 1.2f * k), alpha(hot, (1.0f - k) * 0.6f * fade), 24);
            draw->AddCircle(m, s * (1.0f + 1.8f * k), alpha(IM_COL32_WHITE, (1.0f - k) * 0.7f * fade), 32, std::max(1.5f, r * 0.3f));
        }
    }
}

// The original's X-ray: the world sinks into a dark blue, and only the bones that got hurt show
// through the body, glowing white, the broken ones orange with a red-hot fracture.
// The original's HoM grade, measured from its footage: the world goes dark, cool and nearly
// colourless (ground about (30, 32, 50), edges near black), where the skater's own colours wash
// out to blue-grey. Drawn as a cool wash over the whole picture plus a spotlight that falls to
// near black away from the skater.
void draw_grade(ImDrawList *draw, ImVec2 centre, float strength) {
    if (strength <= 0.01f) return;
    const auto display = ImGui::GetIO().DisplaySize;
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(14, 22, 58, static_cast<int>(0.60f * strength * 255.0f)));
    const float h = display.y;
    const float radii[]{0.14f * h, 0.30f * h, 0.58f * h, 1.7f * std::max(display.x, display.y)};
    const float alphas[]{0.0f, 0.45f, 0.82f, 0.95f};
    constexpr int segments = 48;
    const auto uv = ImGui::GetFontTexUvWhitePixel();
    for (int ring = 0; ring < 3; ++ring) {
        draw->PrimReserve(segments * 6, segments * 4);
        for (int k = 0; k < segments; ++k) {
            const float a0 = k * 6.2831853f / segments, a1 = (k + 1) * 6.2831853f / segments;
            const auto at = [&](float r, float angle) { return ImVec2(centre.x + std::cos(angle) * r, centre.y + std::sin(angle) * r); };
            const ImU32 inner = IM_COL32(1, 3, 12, static_cast<int>(alphas[ring] * strength * 255.0f));
            const ImU32 outer = IM_COL32(1, 3, 12, static_cast<int>(alphas[ring + 1] * strength * 255.0f));
            const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
            draw->PrimWriteVtx(at(radii[ring], a0), uv, inner);
            draw->PrimWriteVtx(at(radii[ring], a1), uv, inner);
            draw->PrimWriteVtx(at(radii[ring + 1], a1), uv, outer);
            draw->PrimWriteVtx(at(radii[ring + 1], a0), uv, outer);
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 1));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            draw->PrimWriteIdx(base);
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 2));
            draw->PrimWriteIdx(static_cast<ImDrawIdx>(base + 3));
        }
    }
}

// Scrolling film grain over the whole picture, one noise texel per screen pixel like the original's
// noise pass (f_noise_texture scrolled every frame).
void draw_grain(ImDrawList *draw, float strength) {
    HomArt grain;
    if (strength <= 0.01f || !hom_art("grain", grain)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    const float tile = 128.0f;
    static std::uint32_t seed = 12345u;
    seed = seed * 1664525u + 1013904223u;
    const float ox = -static_cast<float>(seed % 128u), oy = -static_cast<float>((seed >> 8) % 128u);
    const ImU32 colour = IM_COL32(255, 255, 255, static_cast<int>(strength * 255.0f));
    for (float y = oy; y < display.y; y += tile)
        for (float x = ox; x < display.x; x += tile)
            draw->AddImage(grain.texture, ImVec2(x, y), ImVec2(x + tile, y + tile), grain.uv0, grain.uv1, colour);
}

// The original's X-ray: the picture closes in to a dark, grainy spotlight on the skater, and only
// the bones that got hurt show through the body: warm white, the broken ones orange, red at the break.
void draw_xray(ImDrawList *draw, const Result &r, double, float fade, float darkness) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto rig = hall_of_meat::latest_rig();
    if (rig.valid) {
        // The pose was read at the client tick; carry each bone on by the time since, so the X-ray
        // keeps up with the body it is drawn over.
        // The animation behind a read is a frame older than the tick that read it, so lead by the time
        // since the read plus one more frame.
        const float ahead = std::clamp(static_cast<float>(std::chrono::duration<double>(Clock::now() - rig.at).count()) + rig.period + 0.004f, 0.0f, 0.09f);
        for (auto &seg : rig.bones)
            for (std::size_t k = 0; k < 3; ++k) {
                seg.a[k] += std::clamp(seg.va[k], -30.0f, 30.0f) * ahead;
                seg.b[k] += std::clamp(seg.vb[k], -30.0f, 30.0f) * ahead;
            }
    }
    BoneProjector projector;
    float fov{};
    const bool camera = rig.valid && live_camera(rig.base, projector.camera, fov);
    if (camera) {
        projector.focal = display.y / (2.0f * std::tan(fov * 3.14159265f / 360.0f));
        projector.centre = ImVec2(display.x * 0.5f, display.y * 0.5f);
    }
    ImVec2 spot(display.x * 0.5f, display.y * 0.5f);
    float spot_depth{};
    if (camera) {
        const auto &hips = rig.bones[4];
        const std::array<float, 3> middle{(hips.a[0] + hips.b[0]) * 0.5f, (hips.a[1] + hips.b[1]) * 0.5f, (hips.a[2] + hips.b[2]) * 0.5f};
        ImVec2 at;
        if (projector.project(middle, at, spot_depth)) spot = at;
    }
    draw_grade(draw, spot, darkness * fade);
    draw_grain(draw, 0.13f * darkness * fade);
    if (!camera) return;
    std::array<bool, hall_of_meat::rig_bones> broken{};
    for (const auto &b : r.broken)
        if (b.bone < broken.size()) broken[b.bone] = true;
    const bool meshes = hom_bones_ready();
    if (hall_of_meat::rig_upright().load()) return; // getting up: the original's X-ray is over
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < hall_of_meat::rig_bones; ++i) {
            const float damage = r.damage[i];
            if (!broken[i] && damage < 0.5f) continue;
            if (broken[i] != (pass == 1)) continue;
            const auto &seg = rig.bones[i];
            if (!meshes) {
                ImVec2 a, b;
                float da{}, db{};
                if (!projector.project(seg.a, a, da) || !projector.project(seg.b, b, db)) continue;
                const float px = std::clamp(seg.radius * projector.focal * 2.0f / (da + db), 1.5f, 80.0f);
                draw_bone(draw, i, a, b, px, broken[i], 1.0f, fade);
                continue;
            }
            const float strength = broken[i] ? 1.0f : std::clamp((damage - 0.5f) * 2.0f + 0.4f, 0.4f, 1.0f);
            // Skate 3's colours, measured from the original: hurt (223, 212, 214), broken (198, 87, 64),
            // fracture (158, 43, 23), after its bone map; these tints give those through the map.
            const ImU32 tint = broken[i] ? IM_COL32(235, 103, 75, 255) : IM_COL32(255, 244, 246, 255);
            const auto &facing = seg.has_front && hall_of_meat::roll_from_joints().load() ? seg.front : rig.forward;
            draw_hom_bone(draw, projector, i, seg.a, seg.b, facing, tint, strength * fade, broken[i] ? 0.85f : 0.0f);
        }
}

std::string clock_text(double seconds) {
    const int total = std::max(0, static_cast<int>(std::ceil(seconds)));
    return std::format("{:02}:{:02}", total / 60, total % 60);
}
void right_text(ImDrawList *draw, ImFont *font, float size, float right, float y, ImU32 colour, const std::string &text) {
    const auto e = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    shadowed(draw, font, size, ImVec2(right - e.x, y), colour, text);
}

// Timer at the bottom centre, objectives and progress bottom right, "Go!" and result messages.
void draw_session(ImDrawList *draw, const Session &ses, float scale, double now) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto *font = ImGui::GetFont();
    const float bottom = display.y * 0.925f;
    if (ses.active) {
        const double remaining = ses.finished ? 0.0 : ses.ends - now;
        const auto timer = clock_text(remaining);
        const float ts = 30.0f * scale;
        const auto e = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, timer.c_str());
        shadowed(draw, font, ts, ImVec2(display.x * 0.5f - e.x * 0.5f, bottom - ts), remaining < 10.0 ? IM_COL32(255, 120, 110, 255) : white, timer);

        const float right = display.x * 0.955f, line = 25.0f * scale, box = 15.0f * scale;
        const float lines_h = line * static_cast<float>(ses.challenge.objectives.size());
        float y = bottom - lines_h;
        for (const auto &o : ses.challenge.objectives) {
            right_text(draw, font, 17.0f * scale, right - box - 8.0f * scale, y, white, o.text);
            const ImVec2 a(right - box, y + 3.0f * scale), b(right, y + 3.0f * scale + box);
            draw->AddRect(a, b, alpha(cyan, 0.9f), 2.0f * scale, 0, 1.5f * scale);
            if (o.done) {
                draw->AddLine(ImVec2(a.x + 3.0f * scale, a.y + box * 0.55f), ImVec2(a.x + box * 0.42f, b.y - 3.0f * scale), cyan, 2.5f * scale);
                draw->AddLine(ImVec2(a.x + box * 0.42f, b.y - 3.0f * scale), ImVec2(b.x - 2.5f * scale, a.y + 2.5f * scale), cyan, 2.5f * scale);
            }
            y += line;
        }
        right_text(draw, font, 22.0f * scale, right, bottom - lines_h - 28.0f * scale, white, ses.challenge.title);
        right_text(draw, font, 15.0f * scale, right, bottom - lines_h - 50.0f * scale, alpha(white, 0.8f),
                   std::format("{} / {}", with_commas(ses.total), with_commas(ses.challenge.goal)));
    }
    // "Go!" for the first moments, then the result.
    std::string message = ses.message;
    if (message.empty() && ses.active && now - ses.started < 1.6) message = "Go!";
    if (!message.empty() && (ses.message_until == 0 || now < ses.message_until)) {
        const float ms = 64.0f * scale;
        const auto e = font->CalcTextSizeA(ms, FLT_MAX, 0.0f, message.c_str());
        shadowed(draw, font, ms, ImVec2((display.x - e.x) * 0.5f, display.y * 0.30f), white, message);
    }
}
} // namespace

// Game thread, once per client tick after the trainer has published its telemetry.
void hall_of_meat_tick() {
    auto &s = state();
    const auto telemetry = trainer::telemetry();
    std::lock_guard lock(s.mutex);
    if (!telemetry.skater) {
        if (s.tracker.phase() == Phase::bailing) s.tracker.reset();
        s.last_time = -1;
        return;
    }
    const double now = clock_seconds();
    if (now == s.last_time) return;
    s.last_time = now;
    if (s.session.active && !s.session.finished && now >= s.session.ends) {
        s.session.finished = true;
        s.session.message = "Out of Time";
        s.session.message_until = now + 5.0;
    }
    Sample in;
    in.time = now;
    in.position = telemetry.position;
    in.speed = telemetry.speed;
    in.vertical = telemetry.vertical;
    in.airborne = telemetry.airborne;
    in.physics_state = telemetry.physics_state;
    in.heading = telemetry.heading;
    if (const auto rig = hall_of_meat::latest_rig(); rig.valid) {
        in.bones_valid = true;
        in.upright = hall_of_meat::rig_upright().load();
        for (std::size_t i = 0; i < hall_of_meat::rig_bones; ++i)
            for (std::size_t k = 0; k < 3; ++k) in.bone_centres[i][k] = (rig.bones[i].a[k] + rig.bones[i].b[k]) * 0.5f;
    }
    // Calibration log: every physics state change, and a heartbeat, so a missed bail can be traced.
    if (in.physics_state != s.last_state && s.tracker.phase() != Phase::bailing)
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: state {} -> {} ({:.1f} km/h, airborne {}).",
                     s.last_state, in.physics_state, in.speed * 3.6f, in.airborne);
    if (now - s.heartbeat > 10.0) {
        s.heartbeat = now;
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: watching, state {}, skeleton {}.", in.physics_state,
                     in.bones_valid ? "read" : "not read");
    }
    if (s.tracker.phase() == Phase::bailing && in.physics_state != s.last_state)
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: physics state {} -> {} at {:.1f} km/h.",
                     s.last_state, in.physics_state, in.speed * 3.6f);
    s.last_state = in.physics_state;
    const bool was_bailing = s.tracker.phase() == Phase::bailing;
    const bool done = s.tracker.update(in);
    hall_of_meat::hide_game_ui().store(s.tracker.phase() == Phase::bailing || now < s.visible_until);
    if (!was_bailing && s.tracker.phase() == Phase::bailing) {
        s.started = now;
        s.shown_total = 0;
        // Calibration: the X-ray skull and pelvis against the skater's own position.
        if (const auto rig = hall_of_meat::latest_rig(); rig.valid)
            logging::log(logging::Level::info, logging::Channel::assets,
                         "Hall Of Meat: skater ({:.2f}, {:.2f}, {:.2f}), skull ({:.2f}, {:.2f}, {:.2f}), hips ({:.2f}, {:.2f}, {:.2f}).",
                         in.position[0], in.position[1], in.position[2], rig.bones[0].a[0], rig.bones[0].a[1], rig.bones[0].a[2],
                         rig.bones[4].a[0], rig.bones[4].a[1], rig.bones[4].a[2]);
        else
            logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: skeleton not readable at bail start.");
    }
    if (done) {
        const auto &r = s.tracker.result();
        s.visible_until = now + linger_seconds + fade_seconds;
        s.best = std::max(s.best, r.total);
        if (s.session.active && !s.session.finished) {
            auto &ses = s.session;
            ses.total += r.total;
            ses.bones += static_cast<int>(r.broken.size());
            const auto &list = bones();
            for (auto &o : ses.challenge.objectives) {
                if (o.done) continue;
                if (o.kind == 0) o.done = r.total >= o.target;
                else if (o.kind == 1) o.done = static_cast<int>(r.broken.size()) >= o.target;
                else if (o.kind == 3) o.done = ses.bones >= o.target;
                else if (o.kind == 2)
                    for (const auto &b : r.broken) {
                        const std::string region = list[b.bone].region, name = lower_case(list[b.bone].name);
                        if ((!o.region.empty() && region == o.region) || (!o.word.empty() && name.find(o.word) != std::string::npos)) o.done = true;
                    }
            }
            const bool all = std::all_of(ses.challenge.objectives.begin(), ses.challenge.objectives.end(), [](const Objective &o) { return o.done; });
            if (all && ses.total >= ses.challenge.goal) {
                ses.finished = ses.won = true;
                ses.message = "Challenge Complete!";
                ses.message_until = now + 5.0;
            }
        }
        logging::log(logging::Level::info, logging::Channel::assets,
                     "Hall Of Meat: bail {} scored {} (bones {} {}, air {:.2f}s {}, drop {:.1f}m {}, time {:.2f}s {}, speed {:.1f}m/s {}, rot {:.0f} {}) - {}.",
                     r.serial, r.total, r.broken.size(), r.bone_points, r.air_time, r.air_points, r.drop, r.drop_points, r.duration,
                     r.duration_points, r.peak_speed, r.speed_points, r.rotation, r.rotation_points, r.title);
    }
}

bool hall_of_meat_hud_pending() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.tracker.phase() == Phase::bailing || clock_seconds() < s.visible_until || s.session.active;
}

std::string hall_of_meat_command(std::string_view verb, const std::vector<std::string> &words) {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    const double now = clock_seconds();
    if (verb == "start") {
        const auto &list = challenges();
        std::size_t index = 0;
        if (!words.empty()) {
            const auto number = std::atoi(words[0].c_str());
            if (number >= 1 && number <= static_cast<int>(list.size())) {
                index = static_cast<std::size_t>(number - 1);
            } else {
                std::string wanted;
                for (const auto &w : words) wanted += (wanted.empty() ? "" : " ") + w;
                wanted = lower_case(wanted);
                for (std::size_t i = 0; i < list.size(); ++i)
                    if (lower_case(list[i].title).find(wanted) != std::string::npos) index = i;
            }
        }
        s.session = {};
        s.session.active = true;
        s.session.challenge = list[index];
        s.session.started = now;
        s.session.ends = now + s.session.challenge.minutes * 60.0;
        return "Hall Of Meat: started \"" + list[index].title + "\".";
    }
    if (verb == "roll") {
        if (!words.empty()) hall_of_meat::roll_from_joints().store(words[0] != "0");
        return std::string("Hall Of Meat: bone roll from ") + (hall_of_meat::roll_from_joints().load() ? "the joints." : "the body forward.");
    }
    if (verb == "rig") {
        hall_of_meat::rig_probe_requested().store(true);
        return "Hall Of Meat: reading the skeleton, see ReSkate.log.";
    }
    if (verb == "stop") {
        s.session = {};
        return "Hall Of Meat: stopped.";
    }
    if (verb == "list") {
        std::string out = "Hall Of Meat challenges:";
        int i = 1;
        for (const auto &c : challenges()) out += std::format(" {}={}", i++, c.title);
        return out;
    }
    if (verb == "status") {
        if (!s.session.active) return std::format("Hall Of Meat: no challenge running. Best bail {}.", s.best);
        return std::format("Hall Of Meat: \"{}\", {} of {}, {} left{}.", s.session.challenge.title, s.session.total, s.session.challenge.goal,
                           clock_text(s.session.finished ? 0.0 : s.session.ends - now),
                           s.session.finished ? (s.session.won ? ", complete" : ", failed") : "");
    }
    return "Hall Of Meat: start [number|name], stop, list, status.";
}

void draw_hall_of_meat_hud() {
    auto &s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    auto *draw = ImGui::GetBackgroundDrawList();
    std::lock_guard lock(s.mutex);
    const double now = clock_seconds();
    const bool live = s.tracker.phase() == Phase::bailing;
    if (s.session.active || !s.session.message.empty()) draw_session(draw, s.session, scale, now);
    if (!live && now >= s.visible_until) return;
    const Result &r = live ? s.tracker.live() : s.tracker.result();
    float fade = 1.0f;
    if (!live) fade = static_cast<float>(std::clamp((s.visible_until - now) / fade_seconds, 0.0, 1.0));
    // The score counts up toward its target.
    const float target = static_cast<float>(r.total);
    s.shown_total += (target - s.shown_total) * std::min(1.0f, ImGui::GetIO().DeltaTime * 8.0f);
    if (std::abs(target - s.shown_total) < 1.0f) s.shown_total = target;
    {
        // Darkness: deep once the body is settling (as the original reveals its X-ray), lighter mid-tumble.
        // The reveal closes in once the body is settling, as in the original.
        const float dark_target = live ? ((s.tracker.live().duration > 0.7f) ? 1.0f : 0.5f) : 0.0f;
        s.darkness += (dark_target - s.darkness) * std::min(1.0f, ImGui::GetIO().DeltaTime * 4.0f);
        if (live || s.darkness > 0.02f) draw_xray(draw, r, now, live ? 1.0f : fade, s.darkness);
    }
    draw_block(draw, r, s.shown_total, fade, scale, now, s.started);
}
} // namespace dingosdk::overlay
