#include "hall_of_meat_hud.h"
#include "hom_core.h"
#include "Engine/Core/Log/logging.h"
#include "Extension/Trainer/trainer.h"
#include "Extension/UI/skate_theme.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <chrono>
#include <imgui.h>
#include <mutex>
#include <string>

namespace dingosdk::overlay {
namespace {
namespace theme = dingosdk::skate_theme;
using namespace dingosdk::hall_of_meat;

constexpr double result_seconds = 9.0;

struct State {
    std::mutex mutex; // the game thread feeds the tracker, the presentation thread draws it
    Tracker tracker;
    double result_until{}; // steady-clock seconds
    int best{};
    int session_total{};
    int bails{};
    std::uint32_t last_state{};
    double last_time{-1};
};
State &state() {
    static State value;
    return value;
}

std::string with_commas(int value) {
    auto text = std::to_string(value);
    for (int i = static_cast<int>(text.size()) - 3; i > 0; i -= 3) text.insert(static_cast<std::size_t>(i), ",");
    return text;
}

double clock_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
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
    Sample in;
    in.time = now;
    in.position = telemetry.position;
    in.speed = telemetry.speed;
    in.vertical = telemetry.vertical;
    in.airborne = telemetry.airborne;
    in.physics_state = telemetry.physics_state;
    // Calibration log: the physics states a bail passes through, so the thresholds can be tuned.
    if (s.tracker.phase() == Phase::bailing && in.physics_state != s.last_state)
        logging::log(logging::Level::info, logging::Channel::assets, "Hall Of Meat: physics state {} -> {} at {:.1f} km/h.",
                     s.last_state, in.physics_state, in.speed * 3.6f);
    s.last_state = in.physics_state;
    if (s.tracker.update(in)) {
        const auto &r = s.tracker.result();
        s.result_until = now + result_seconds;
        s.best = std::max(s.best, r.total);
        s.session_total += r.total;
        ++s.bails;
        logging::log(logging::Level::info, logging::Channel::assets,
                     "Hall Of Meat: bail {} scored {} ({} bones, {} impacts, {:.1f} m, {:.1f} s, biggest hit {:.1f} m/s) - {}.", r.serial,
                     r.total, r.broken.size(), r.impacts, r.distance, r.duration, r.biggest_hit, r.title);
    }
}

namespace {
void plate(ImDrawList *draw, ImVec2 a, ImVec2 b, float scale, ImU32 accent) {
    draw->AddRectFilled(a, b, theme::tile, 6.0f * scale);
    draw->AddRectFilled(a, ImVec2(a.x + 5.0f * scale, b.y), accent, 3.0f * scale);
}
void centered(ImDrawList *draw, ImFont *font, float size, float cx, float y, ImU32 colour, const std::string &text) {
    const auto extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    draw->AddText(font, size, ImVec2(cx - extent.x * 0.5f, y), colour, text.c_str());
}

void draw_live(ImDrawList *draw, const Result &r, float scale) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto *font = ImGui::GetFont();
    const float width = 420.0f * scale, height = 118.0f * scale;
    const ImVec2 a((display.x - width) * 0.5f, 28.0f * scale);
    plate(draw, a, ImVec2(a.x + width, a.y + height), scale, theme::danger);
    const float cx = a.x + width * 0.5f;
    centered(draw, font, 20.0f * scale, cx, a.y + 8.0f * scale, theme::danger, "HALL OF MEAT");
    centered(draw, font, 44.0f * scale, cx, a.y + 30.0f * scale, theme::white, with_commas(r.total));
    centered(draw, font, 18.0f * scale, cx, a.y + 84.0f * scale, theme::grey_text,
             std::format("{} bones   {:.1f} m   {:.1f} s", r.broken.size(), r.distance, r.air_time));
}

void draw_result(ImDrawList *draw, const Result &r, int best, float scale) {
    const auto display = ImGui::GetIO().DisplaySize;
    auto *font = ImGui::GetFont();
    const auto &list = bones();
    const float width = 520.0f * scale;
    const float row = 22.0f * scale;
    const std::size_t shown = std::min<std::size_t>(r.broken.size(), 8);
    const float height = 190.0f * scale + row * static_cast<float>(shown) + (r.broken.size() > shown ? row : 0.0f);
    const ImVec2 a(display.x * 0.5f - width * 0.5f, display.y * 0.18f);
    draw->PushClipRectFullScreen();
    plate(draw, a, ImVec2(a.x + width, a.y + height), scale, theme::danger);
    const float cx = a.x + width * 0.5f;
    centered(draw, font, 22.0f * scale, cx, a.y + 10.0f * scale, theme::danger, r.title);
    centered(draw, font, 48.0f * scale, cx, a.y + 36.0f * scale, theme::white, with_commas(r.total));
    if (r.total >= best && r.total > 0) centered(draw, font, 18.0f * scale, cx, a.y + 92.0f * scale, theme::bar, "SESSION BEST");
    else centered(draw, font, 18.0f * scale, cx, a.y + 92.0f * scale, theme::grey_text, std::format("best {}", with_commas(best)));
    centered(draw, font, 16.0f * scale, cx, a.y + 116.0f * scale, theme::grey_text,
             std::format("bones {}   distance {}   air {}   height {}   speed {}   x{:.1f}", r.bone_points, r.distance_points, r.air_points,
                         r.height_points, r.speed_points, static_cast<float>(r.multiplier_tenths) / 10.0f));
    float y = a.y + 146.0f * scale;
    for (std::size_t i = 0; i < shown; ++i) {
        const auto &b = list[r.broken[i].bone];
        draw->AddText(font, 17.0f * scale, ImVec2(a.x + 22.0f * scale, y), theme::white, std::format("{}  ({})", b.name, b.region).c_str());
        const auto points = std::format("+{}", b.points);
        const auto extent = font->CalcTextSizeA(17.0f * scale, FLT_MAX, 0.0f, points.c_str());
        draw->AddText(font, 17.0f * scale, ImVec2(a.x + width - 22.0f * scale - extent.x, y), theme::bar, points.c_str());
        y += row;
    }
    if (r.broken.size() > shown)
        draw->AddText(font, 16.0f * scale, ImVec2(a.x + 22.0f * scale, y), theme::grey_text,
                      std::format("and {} more", r.broken.size() - shown).c_str());
    draw->PopClipRect();
}
} // namespace

bool hall_of_meat_hud_pending() {
    auto &s = state();
    std::lock_guard lock(s.mutex);
    return s.tracker.phase() == Phase::bailing || clock_seconds() < s.result_until;
}

void draw_hall_of_meat_hud() {
    auto &s = state();
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const float scale = std::clamp(display.y / 1080.0f, 0.75f, 2.5f);
    auto *draw = ImGui::GetForegroundDrawList();
    std::lock_guard lock(s.mutex);
    if (s.tracker.phase() == Phase::bailing) draw_live(draw, s.tracker.live(), scale);
    else if (clock_seconds() < s.result_until) draw_result(draw, s.tracker.result(), s.best, scale);
}
} // namespace dingosdk::overlay
