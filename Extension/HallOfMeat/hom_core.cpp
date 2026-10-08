#include "hom_core.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::hall_of_meat {
const std::vector<Bone> &bones() {
    // Points follow the Skate games' habit of paying more for the bones that hurt most to lose.
    // The 19 bones of the original's X-ray skeleton (Skate 3 `dem_bones_hom`), in hom_rig.h order.
    static const std::vector<Bone> list{
        {"Skull", "Head", 250, 0.35f, 5},          {"Neck", "Spine", 400, 0.2f, 2},
        {"Rib Cage", "Torso", 300, 0.55f, 7},      {"Lower Spine", "Spine", 300, 0.3f, 4},
        {"Hips", "Torso", 250, 0.35f, 5},          {"Left Bicep", "Arm", 160, 0.45f, 5},
        {"Right Bicep", "Arm", 160, 0.45f, 5},     {"Left Forearm", "Arm", 130, 0.55f, 6},
        {"Right Forearm", "Arm", 130, 0.55f, 6},   {"Left Hand", "Arm", 100, 0.7f, 7},
        {"Right Hand", "Arm", 100, 0.7f, 7},       {"Left Thigh", "Leg", 280, 0.25f, 4},
        {"Right Thigh", "Leg", 280, 0.25f, 4},     {"Left Calf", "Leg", 160, 0.45f, 5},
        {"Right Calf", "Leg", 160, 0.45f, 5},      {"Left Ankle", "Leg", 100, 0.65f, 6},
        {"Right Ankle", "Leg", 100, 0.65f, 6},     {"Left Toes", "Leg", 60, 0.75f, 6},
        {"Right Toes", "Leg", 60, 0.75f, 6},
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

// With the skeleton: a bone breaks when its own middle loses speed suddenly (it hit something),
// harder for the sturdy ones. This is what puts the break where the body actually struck.
void Tracker::break_bones_from_rig(const Sample &previous, const Sample &now, double dt) {
    const auto &list = bones();
    std::array<std::array<float, 3>, 19> velocity{};
    for (std::size_t i = 0; i < 19; ++i)
        for (std::size_t k = 0; k < 3; ++k)
            velocity[i][k] = static_cast<float>((now.bone_centres[i][k] - previous.bone_centres[i][k]) / dt);
    if (have_bone_velocity_) {
        for (std::size_t i = 0; i < list.size() && i < 19; ++i) {
            if (is_broken_[i]) continue;
            // Speed the bone's middle lost this tick: a hit stops it, a swing only turns it.
            const auto length = [](const std::array<float, 3> &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); };
            const float change = length(bone_velocity_[i]) - length(velocity[i]);
            // Teleports and respawns move the whole body at once: not a hit.
            if (change > 60.0f) continue;
            const float needed = config_.bone_break_speed * (1.35f - list[i].fragility);
            if (now.time - start_.time < 0.15) continue; // the wipeout's own first jolt
            if (change >= needed) {
                is_broken_[i] = true;
                live_.broken.push_back({i, change, now.time});
                live_.biggest_hit = std::max(live_.biggest_hit, change);
            }
        }
    }
    bone_velocity_ = velocity;
    have_bone_velocity_ = true;
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
            live_.broken.push_back({index, drop, previous_.time});
        }
    }
}

void Tracker::score(Result &r) const {
    const auto &list = bones();
    int base = 0;
    for (const auto &b : r.broken) base += list[b.bone].points;
    r.bone_points = base;
    const auto cap = [&](float value) { return std::min(config_.metric_cap, static_cast<int>(std::max(0.0f, value))); };
    r.air_points = cap(r.air_time * config_.air_scale);
    r.drop_points = cap(r.drop * config_.drop_scale);
    r.duration_points = static_cast<int>(std::max(0.0f, r.duration - config_.duration_start) * config_.duration_scale);
    r.speed_points = static_cast<int>(r.peak_speed * 3.6f * config_.speed_scale);
    r.rotation_points = static_cast<int>(r.rotation * config_.rotation_scale);
    r.total = r.bone_points + r.air_points + r.drop_points + r.duration_points + r.speed_points + r.rotation_points;
    r.title = title_for(r.total, r.broken.size());
}

bool Tracker::update(const Sample &s) {
    bool finished = false;
    if (phase_ == Phase::finished) phase_ = Phase::idle;
    if (phase_ == Phase::idle) {
        if (s.physics_state == config_.wipeout_state && (!have_previous_ || previous_.physics_state != config_.wipeout_state)) {
            phase_ = Phase::bailing;
            live_ = {};
            live_.serial = ++serial_;
            rng_ = 0xC0FFEEull * live_.serial + 17;
            is_broken_.assign(bones().size(), false);
            have_bone_velocity_ = false;
            start_ = s;
            slow_since_ = -1;
            air_start_ = -1;
            live_.peak_speed = s.speed;
        }
    } else if (have_previous_) {
        const double dt = s.time - previous_.time;
        if (dt > 0 && dt < 0.25) {
            const float drop = previous_.speed - s.speed;
            if (s.bones_valid && previous_.bones_valid) {
                if (drop >= config_.impact_threshold) ++live_.impacts;
                break_bones_from_rig(previous_, s, dt);
            } else {
                have_bone_velocity_ = false;
                if (drop >= config_.impact_threshold) register_impact(drop);
            }
            if (s.airborne) {
                if (air_start_ < 0) air_start_ = previous_.time;
            } else if (air_start_ >= 0) {
                live_.air_time += static_cast<float>(s.time - air_start_);
                air_start_ = -1;
            }
        }
        live_.distance = dist(s.position, start_.position);
        live_.peak_speed = std::max(live_.peak_speed, s.speed);
        live_.peak_height = std::max(live_.peak_height, s.position[1] - start_.position[1]);
        live_.drop = std::max(live_.drop, start_.position[1] - s.position[1]);
        {
            float turn = s.heading - previous_.heading;
            while (turn > 180.0f) turn -= 360.0f;
            while (turn < -180.0f) turn += 360.0f;
            if (dt > 0 && dt < 0.25) live_.rotation += std::abs(turn);
        }
        live_.duration = static_cast<float>(s.time - start_.time);
        if (s.speed < config_.settle_speed) {
            if (slow_since_ < 0) slow_since_ = s.time;
        } else {
            slow_since_ = -1;
        }
        const bool settled = slow_since_ >= 0 && s.time - slow_since_ >= config_.settle_time;
        // With the skeleton: over once the skater is back on their feet. Without it: out of the
        // wipeout state and slow.
        const bool recovered = s.bones_valid ? (s.upright && live_.duration > 0.6f)
                                             : (s.physics_state != config_.wipeout_state && live_.duration > 0.5f && s.speed < 2.0f);
        if (settled || recovered || live_.duration > config_.max_duration) {
            if (air_start_ >= 0) live_.air_time += static_cast<float>(s.time - air_start_);
            if (live_.duration >= config_.min_duration) {
                score(live_);
                last_ = live_;
                phase_ = Phase::finished;
                finished = true;
            } else {
                phase_ = Phase::idle;
            }
        }
    }
    if (phase_ == Phase::bailing) score(live_);
    previous_ = s;
    have_previous_ = true;
    return finished;
}
} // namespace dingosdk::hall_of_meat
