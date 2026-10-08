#include "hall_of_meat_hud.h"
#include "hom_art.h"
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
    auto *font = ImGui::GetFont();
    const float left = display.x * 0.083f, right = display.x * 0.23f, bottom = display.y * 0.925f;
    const float width = right - left;
    const float intro = std::clamp(static_cast<float>((now - started) / 0.25), 0.0f, 1.0f);
    const float a = fade * intro;
    const float slide = (1.0f - intro) * -24.0f * scale;
    const float cx = left + width * 0.5f + slide;

    // Score and mark.
    const float score_size = 54.0f * scale;
    const auto score = with_commas(static_cast<int>(total_shown));
    const auto extent = font->CalcTextSizeA(score_size, FLT_MAX, 0.0f, score.c_str());
    const float score_y = bottom - score_size - 6.0f * scale;
    shadowed(draw, font, score_size, ImVec2(cx - extent.x * 0.5f, score_y), alpha(white, a), score);
    draw->AddRectFilled(ImVec2(left + slide, bottom), ImVec2(right + slide, bottom + 2.0f * scale), alpha(IM_COL32(170, 220, 235, 190), a));

    const float hom_size = 21.0f * scale;
    const auto hom = std::string("Hall of Meat");
    const auto hom_extent = font->CalcTextSizeA(hom_size, FLT_MAX, 0.0f, hom.c_str());
    const float hom_y = score_y - hom_size - 2.0f * scale;
    shadowed(draw, font, hom_size, ImVec2(cx - hom_extent.x * 0.5f, hom_y), alpha(IM_COL32(236, 236, 236, 255), a), hom);

    const float logo_h = 46.0f * scale;
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

    const float row_h = 36.0f * scale, text_size = 24.0f * scale;
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
            draw->AddCircleFilled(m, s * (1.0f + 3.0f * k), alpha(hot, (1.0f - k) * 0.55f * fade), 24);
            draw->AddCircle(m, s * (1.5f + 5.0f * k), alpha(IM_COL32_WHITE, (1.0f - k) * 0.8f * fade), 32, std::max(2.0f, r * 0.4f));
        }
    }
}

void draw_xray(ImDrawList *draw, const Result &r, double now, float fade) {
    const auto rig = hall_of_meat::latest_rig();
    if (!rig.valid) return;
    Projector project;
    float fov{};
    if (!live_camera(rig.base, project.m, fov)) return;
    const auto display = ImGui::GetIO().DisplaySize;
    project.focal = display.y / (2.0f * std::tan(fov * 3.14159265f / 360.0f));
    project.centre = ImVec2(display.x * 0.5f, display.y * 0.5f);
    std::array<float, hall_of_meat::rig_bones> broke_at{};
    broke_at.fill(-1.0f);
    for (const auto &b : r.broken)
        if (b.bone < broke_at.size()) broke_at[b.bone] = static_cast<float>(std::max(0.0, now - b.time));
    // Healthy bones first, broken ones on top.
    for (int pass = 0; pass < 2; ++pass)
        for (std::size_t i = 0; i < hall_of_meat::rig_bones; ++i) {
            const bool broken = broke_at[i] >= 0.0f;
            if (broken != (pass == 1)) continue;
            const auto &seg = rig.bones[i];
            ImVec2 a, b;
            float pa{}, pb{};
            if (!project(seg.a, a, pa) || !project(seg.b, b, pb)) continue;
            draw_bone(draw, i, a, b, std::clamp(seg.radius * (pa + pb) * 0.5f, 1.5f, 80.0f), broken, broke_at[i], fade);
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
        for (std::size_t i = 0; i < hall_of_meat::rig_bones; ++i)
            for (std::size_t k = 0; k < 3; ++k) in.bone_centres[i][k] = (rig.bones[i].a[k] + rig.bones[i].b[k]) * 0.5f;
    }
    // Calibration log: the physics states a bail passes through.
    if (s.tracker.phase() == Phase::bailing && in.physics_state != s.last_state)
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: physics state {} -> {} at {:.1f} km/h.",
                     s.last_state, in.physics_state, in.speed * 3.6f);
    s.last_state = in.physics_state;
    const bool was_bailing = s.tracker.phase() == Phase::bailing;
    const bool done = s.tracker.update(in);
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
    draw_vignette(draw, display, fade * (live ? 1.0f : 0.6f));
    draw_xray(draw, r, now, live ? 1.0f : fade);
    draw_block(draw, r, s.shown_total, fade, scale, now, s.started);
}
} // namespace dingosdk::overlay
