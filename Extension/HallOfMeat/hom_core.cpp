#include "hom_core.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::hall_of_meat {
const std::vector<Bone> &bones() {
    // The 19 bones of the original's X-ray skeleton (Skate 3 `dem_bones_hom`), in hom_rig.h order.
    static const std::vector<Bone> list{
        {"Skull", "Head", 0.35f, 5},          {"Neck", "Spine", 0.2f, 2},
        {"Rib Cage", "Torso", 0.55f, 7},      {"Lower Spine", "Spine", 0.3f, 4},
        {"Hips", "Torso", 0.35f, 5},          {"Left Bicep", "Arm", 0.45f, 5},
        {"Right Bicep", "Arm", 0.45f, 5},     {"Left Forearm", "Arm", 0.55f, 6},
        {"Right Forearm", "Arm", 0.55f, 6},   {"Left Hand", "Arm", 0.7f, 7},
        {"Right Hand", "Arm", 0.7f, 7},       {"Left Thigh", "Leg", 0.25f, 4},
        {"Right Thigh", "Leg", 0.25f, 4},     {"Left Calf", "Leg", 0.45f, 5},
        {"Right Calf", "Leg", 0.45f, 5},      {"Left Ankle", "Leg", 0.65f, 6},
        {"Right Ankle", "Leg", 0.65f, 6},     {"Left Toes", "Leg", 0.75f, 6},
        {"Right Toes", "Leg", 0.75f, 6},
    };
    return list;
}

namespace {
std::uint64_t next(std::uint64_t &s) { // splitmix64
    s += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
float unit(std::uint64_t &s) { return static_cast<float>(next(s) >> 40) / static_cast<float>(1 << 24); }
float dist(const std::array<float, 3> &a, const std::array<float, 3> &b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
} // namespace

bool metric_visible(const Result &r, int metric) {
    switch (metric) {
    case 0: return r.rotation >= 1.0f;
    case 1: return r.air_time >= 0.1f;
    case 2: return r.drop >= 0.5f;
    case 3: return r.duration_points > 0;
    case 4: return r.speed_points > 0;
    }
    return false;
}

const char *title_for(int total, std::size_t n) {
    if (n >= 20) return "TOTAL ANNIHILATION";
    if (n >= 12) return "SKELETAL DISASTER";
    if (n >= 7) return "MASSIVE CARNAGE";
    if (n >= 3) return "SERIOUS MEAT";
    if (n >= 1) return "SNAP, CRACKLE";
    return total > 0 ? "JUST A TUMBLE" : "NO DAMAGE";
}

void Tracker::reset() {
    phase_ = Phase::idle;
    live_ = {};
    have_previous_ = false;
    slow_since_ = air_start_ = -1;
}

// Each bone's middle: how much speed it lost over the last ~0.13 s. A hit stops a bone, a swing only
// turns it. Kept every tick (not only during a bail) so a crash can be recognised by its impact.
void Tracker::track_bones(const Sample &previous, const Sample &now, double dt) {
    const auto length = [](const std::array<float, 3> &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
    for (std::size_t k = 0; k + 1 < bone_times_.size(); ++k) bone_times_[k] = bone_times_[k + 1];
    bone_times_.back() = now.time;
    bone_samples_ = std::min<int>(bone_samples_ + 1, static_cast<int>(bone_times_.size()));
    for (std::size_t i = 0; i < 19; ++i) {
        std::array<float, 3> v{};
        for (std::size_t k = 0; k < 3; ++k) v[k] = static_cast<float>((now.bone_centres[i][k] - previous.bone_centres[i][k]) / dt);
        auto &h = bone_speeds_[i];
        for (std::size_t k = 0; k + 1 < h.size(); ++k) h[k] = h[k + 1];
        h.back() = length(v);
    }
    bone_change_ = {};
    if (bone_samples_ < 3) return;
    for (std::size_t i = 0; i < 19; ++i) {
        const auto &h = bone_speeds_[i];
        float before = 0;
        for (std::size_t k = h.size() - static_cast<std::size_t>(bone_samples_); k + 1 < h.size(); ++k)
            if (now.time - bone_times_[k] <= 0.13) before = std::max(before, h[k]);
        const float change = before - h.back();
        // Teleports and respawns move the whole body at once: not a hit.
        if (change > 0 && change <= 60.0f) bone_change_[i] = change;
    }
    float sum = 0;
    for (std::size_t i = 0; i < 19; ++i) sum += bone_speeds_[i].back();
    body_speed_ = sum / 19.0f;
    const float vertical = static_cast<float>((now.bone_centres[4][1] - previous.bone_centres[4][1]) / dt);
    hips_vertical_ += (vertical - hips_vertical_) * std::min(1.0f, static_cast<float>(dt) * 8.0f);
}

// During a bail: a bone breaks when its own middle took a hard enough hit (sturdy bones need more).
void Tracker::apply_bone_hits(const Sample &now) {
    if (now.time - start_.time < 0.15) return; // the wipeout's own first jolt
    const auto &list = bones();
    for (std::size_t i = 0; i < list.size() && i < 19; ++i) {
        const float change = bone_change_[i];
        if (change <= 0) continue;
        const float needed = config_.bone_break_speed * (1.35f - list[i].fragility);
        live_.damage[i] = std::max(live_.damage[i], change / needed);
        if (change >= needed && !is_broken_[i]) {
            is_broken_[i] = true;
            live_.broken.push_back({i, change, now.time});
            live_.biggest_hit = std::max(live_.biggest_hit, change);
        }
    }
}

void Tracker::register_impact(float drop) {
    ++live_.impacts;
    live_.biggest_hit = std::max(live_.biggest_hit, drop);
    const auto &list = bones();
    float total_weight = 0;
    for (const auto &b : list) total_weight += b.weight;
    // Harder hits strike more bones.
    const int strikes = std::clamp(static_cast<int>(drop / 3.0f), 1, 6);
    for (int i = 0; i < strikes; ++i) {
        float pick = unit(rng_) * total_weight;
        std::size_t index = 0;
        for (; index + 1 < list.size(); ++index) {
            if (pick < list[index].weight) break;
            pick -= list[index].weight;
        }
        if (is_broken_[index]) continue;
        const float chance = std::clamp((drop - 3.0f) / 14.0f, 0.08f, 0.97f) * (0.35f + list[index].fragility);
        if (unit(rng_) < chance) {
            is_broken_[index] = true;
            live_.damage[index] = std::max(live_.damage[index], 1.0f);
            live_.broken.push_back({index, drop, previous_.time});
        }
    }
}

void Tracker::score(Result &r) const {
    r.bone_points = config_.bone_value * static_cast<int>(r.broken.size());
    const auto cap = [&](float value) { return std::min(config_.metric_cap, static_cast<int>(std::max(0.0f, value))); };
    r.air_points = cap(r.air_time * config_.air_scale);
    r.drop_points = cap(r.drop * config_.drop_scale);
    r.duration_points = static_cast<int>(std::max(0.0f, r.duration - config_.duration_start) * config_.duration_scale);
    r.speed_points = cap((r.peak_speed * 3.6f - config_.speed_start) * config_.speed_scale);
    r.rotation_points = static_cast<int>(r.rotation * config_.rotation_scale);
    r.total = r.bone_points + r.air_points + r.drop_points + r.duration_points + r.speed_points + r.rotation_points;
    r.title = title_for(r.total, r.broken.size());
}

bool Tracker::update(const Sample &s) {
    bool finished = false;
    if (phase_ == Phase::finished) phase_ = Phase::idle;
    {
        const double step = s.time - previous_.time;
        if (have_previous_ && s.bones_valid && previous_.bones_valid && step > 0 && step < 0.25) {
            track_bones(previous_, s, step);
        } else {
            bone_samples_ = 0;
            bone_change_ = {};
        }
    }
    if (phase_ == Phase::idle) {
        // A bail starts in one of two ways:
        //  - the game's wipeout physics state, at once;
        //  - an off-board crash: the body hits something hard (its chest or hips stop suddenly) and is
        //    then off its feet for a moment. Many slams, and every crash taken on foot, never enter the
        //    wipeout state. Coming off the board for a hippy jump, a plant or a dive does none of it:
        //    there is no hit, or the skater is back on their feet at once.
        const bool off_board = s.physics_state == config_.wipeout_state || s.physics_state == 504;
        if (s.bones_valid && s.upright) {
            if (upright_idle_since_ < 0) upright_idle_since_ = s.time;
            if (s.time - upright_idle_since_ >= 0.3) armed_ = true;
        } else {
            upright_idle_since_ = -1;
        }
        if (s.bones_valid && s.lying && off_board) {
            if (down_since_ < 0) {
                down_since_ = s.time;
                down_start_ = s;
            }
        } else {
            down_since_ = -1;
        }
        {
            float torso = std::max({bone_change_[2], bone_change_[3], bone_change_[4]}), any = 0;
            for (const float c : bone_change_) any = std::max(any, c);
            if (off_board && (torso >= config_.hit_torso || any >= config_.hit_any)) hit_time_ = s.time;
            for (std::size_t i = 0; i < 19; ++i)
                if (bone_change_[i] > 0 && (s.time - recent_hit_time_[i] > 3.0 || bone_change_[i] >= recent_hit_[i])) {
                    recent_hit_[i] = bone_change_[i];
                    recent_hit_time_[i] = s.time;
                }
        }
        const bool wipeout = s.physics_state == config_.wipeout_state && (!have_previous_ || previous_.physics_state != config_.wipeout_state);
        // On the ground: a spread-eagle glide or a dive is down and can jolt, but it keeps sinking.
        const bool grounded = std::abs(hips_vertical_) < 1.5f;
        const bool thrown = armed_ && grounded && down_since_ >= 0 && s.time - down_since_ >= config_.down_time && hit_time_ >= 0 && s.time - hit_time_ <= 0.8 &&
                            hit_time_ >= down_since_ - 0.4;
        if (wipeout) trigger_ = "wipeout state";
        else if (thrown) trigger_ = "hit and down";
        if (wipeout || thrown) {
            armed_ = false;
            hit_time_ = -1;
            down_since_ = -1;
            phase_ = Phase::bailing;
            live_ = {};
            live_.serial = ++serial_;
            rng_ = 0xC0FFEEull * live_.serial + 17;
            is_broken_.assign(bones().size(), false);
            went_down_ = !s.upright;
            upright_since_ = -1;
            start_ = thrown ? down_start_ : s;
            slow_since_ = -1;
            air_start_ = -1;
            live_.peak_speed = s.speed;
            frozen_ = false;
            still_since_ = -1;
            // The impact that showed this was a crash came before the bail was recognised: count it.
            if (thrown) {
                const auto &list = bones();
                for (std::size_t i = 0; i < list.size() && i < 19; ++i) {
                    if (s.time - recent_hit_time_[i] > 3.0 || recent_hit_[i] <= 0) continue;
                    const float needed = config_.bone_break_speed * (1.35f - list[i].fragility);
                    live_.damage[i] = std::max(live_.damage[i], recent_hit_[i] / needed);
                    if (recent_hit_[i] >= needed && !is_broken_[i]) {
                        is_broken_[i] = true;
                        live_.broken.push_back({i, recent_hit_[i], recent_hit_time_[i]});
                        live_.biggest_hit = std::max(live_.biggest_hit, recent_hit_[i]);
                    }
                }
            }
            recent_hit_ = {};
        }
    } else if (have_previous_) {
        const double dt = s.time - previous_.time;
        const double elapsed = s.time - start_.time;
        {
            const float moving = s.bones_valid ? body_speed_ : s.speed;
            if (moving < config_.still_speed && elapsed > 0.5) {
                if (still_since_ < 0) still_since_ = s.time;
                if (s.time - still_since_ >= config_.still_time) frozen_ = true;
            } else {
                still_since_ = -1;
                // Set off again (rolling down a ramp after a pause): the bail carries on counting.
                if (frozen_ && moving > 2.0f) frozen_ = false;
            }
        }
        if (!frozen_ && dt > 0 && dt < 0.25) {
            const float drop = previous_.speed - s.speed;
            if (s.bones_valid && previous_.bones_valid) {
                if (drop >= config_.impact_threshold) ++live_.impacts;
                apply_bone_hits(s);
            } else if (drop >= config_.impact_threshold) {
                register_impact(drop);
            }
            // In the air: the board says so while riding; off it, the hips are rising or falling freely.
            float hips_vertical = 0;
            if (s.bones_valid && previous_.bones_valid) hips_vertical = static_cast<float>((s.bone_centres[4][1] - previous_.bone_centres[4][1]) / dt);
            const bool flying = s.airborne || (s.bones_valid && std::abs(hips_vertical) > 1.5f);
            if (flying) {
                if (air_start_ < 0) air_start_ = previous_.time;
            } else if (air_start_ >= 0) {
                live_.air_time += static_cast<float>(s.time - air_start_);
                air_start_ = -1;
            }
        }
        if (!frozen_) {
            live_.distance = dist(s.position, start_.position);
            live_.peak_speed = std::max(live_.peak_speed, s.speed);
            live_.peak_height = std::max(live_.peak_height, s.position[1] - start_.position[1]);
            live_.drop = std::max(live_.drop, start_.position[1] - s.position[1]);
            float turn = s.heading - previous_.heading;
            while (turn > 180.0f) turn -= 360.0f;
            while (turn < -180.0f) turn += 360.0f;
            // Only while the body is really moving: a still body's heading jitters.
            if (dt > 0 && dt < 0.25 && (s.bones_valid ? body_speed_ : s.speed) > 1.0f) live_.rotation += std::abs(turn);
            live_.duration = static_cast<float>(elapsed);
        } else if (air_start_ >= 0) {
            live_.air_time += static_cast<float>(s.time - air_start_);
            air_start_ = -1;
        }
        if (s.speed < config_.settle_speed) {
            if (slow_since_ < 0) slow_since_ = s.time;
        } else {
            slow_since_ = -1;
        }
        // Lying still is not the end while the skeleton is read: the X-ray stays until the skater is up.
        const bool settled = !s.bones_valid && slow_since_ >= 0 && s.time - slow_since_ >= config_.settle_time;
        // With the skeleton: over once the skater is back on their feet. Without it: out of the
        // wipeout state and slow.
        if (s.bones_valid && !s.upright) went_down_ = true;
        if (s.bones_valid && s.upright) {
            if (upright_since_ < 0) upright_since_ = s.time;
        } else {
            upright_since_ = -1;
        }
        // Back on the board ends it at once.
        const bool riding = s.physics_state >= 100 && s.physics_state < 300 && elapsed > 0.3;
        const bool recovered = riding || (s.bones_valid ? (went_down_ && upright_since_ >= 0 && s.time - upright_since_ >= 0.4 && s.speed < 3.0f)
                                                       : (s.physics_state != config_.wipeout_state && elapsed > 0.5 && s.speed < 2.0f));
        if (settled || recovered || elapsed > config_.max_duration) {
            if (air_start_ >= 0) live_.air_time += static_cast<float>(s.time - air_start_);
            air_start_ = -1;
            if (elapsed >= config_.min_duration) {
                score(live_);
                last_ = live_;
                phase_ = Phase::finished;
                finished = true;
            } else {
                phase_ = Phase::idle;
            }
        }
    }
    if (phase_ == Phase::bailing) {
        score(live_);
        for (int k = 0; k < 5; ++k)
            if (live_.shown_at[k] < 0 && metric_visible(live_, k)) live_.shown_at[k] = s.time;
    }
    previous_ = s;
    have_previous_ = true;
    return finished;
}
} // namespace dingosdk::hall_of_meat
