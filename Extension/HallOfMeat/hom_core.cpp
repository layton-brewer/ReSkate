#include "hom_core.h"
#include <algorithm>
#include <cmath>

namespace dingosdk::hall_of_meat {
const std::vector<Bone> &bones() {
    // Points follow the Skate games' habit of paying more for the bones that hurt most to lose.
    static const std::vector<Bone> list{
        {"Skull", "Head", 250, 0.35f, 5},          {"Jaw", "Head", 150, 0.55f, 4},
        {"Nose", "Head", 60, 0.7f, 4},             {"Cheekbone", "Head", 90, 0.6f, 4},
        {"Neck", "Spine", 400, 0.2f, 2},           {"Upper spine", "Spine", 300, 0.3f, 4},
        {"Lower spine", "Spine", 300, 0.3f, 4},    {"Tailbone", "Spine", 120, 0.6f, 5},
        {"Pelvis", "Torso", 250, 0.35f, 5},        {"Left rib", "Torso", 70, 0.65f, 6},
        {"Right rib", "Torso", 70, 0.65f, 6},      {"Sternum", "Torso", 130, 0.45f, 3},
        {"Left collarbone", "Arm", 110, 0.6f, 5},  {"Right collarbone", "Arm", 110, 0.6f, 5},
        {"Left humerus", "Arm", 160, 0.45f, 5},    {"Right humerus", "Arm", 160, 0.45f, 5},
        {"Left elbow", "Arm", 100, 0.55f, 5},      {"Right elbow", "Arm", 100, 0.55f, 5},
        {"Left forearm", "Arm", 130, 0.55f, 6},    {"Right forearm", "Arm", 130, 0.55f, 6},
        {"Left wrist", "Arm", 100, 0.7f, 7},       {"Right wrist", "Arm", 100, 0.7f, 7},
        {"Left femur", "Leg", 280, 0.25f, 4},      {"Right femur", "Leg", 280, 0.25f, 4},
        {"Left knee", "Leg", 140, 0.5f, 5},        {"Right knee", "Leg", 140, 0.5f, 5},
        {"Left tibia", "Leg", 160, 0.45f, 5},      {"Right tibia", "Leg", 160, 0.45f, 5},
        {"Left ankle", "Leg", 100, 0.65f, 6},      {"Right ankle", "Leg", 100, 0.65f, 6},
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
            live_.broken.push_back({index, drop});
        }
    }
}

void Tracker::score(Result &r) const {
    const auto &list = bones();
    int base = 0;
    for (const auto &b : r.broken) base += list[b.bone].points;
    r.bone_points = base;
    r.distance_points = static_cast<int>(r.distance * config_.distance_scale);
    r.height_points = static_cast<int>(r.peak_height * config_.height_scale);
    r.air_points = static_cast<int>(r.air_time * config_.air_scale);
    r.speed_points = static_cast<int>(r.peak_speed * 3.6f * config_.speed_scale);
    // Every bone past the first adds a tenth, so a shattered skeleton is worth far more than the sum.
    r.multiplier_tenths = 10 + std::max<int>(0, static_cast<int>(r.broken.size()) - 1);
    const int raw = r.bone_points + r.distance_points + r.height_points + r.air_points + r.speed_points;
    r.total = raw * r.multiplier_tenths / 10;
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
            start_ = s;
            slow_since_ = -1;
            air_start_ = -1;
            live_.peak_speed = s.speed;
        }
    } else if (have_previous_) {
        const double dt = s.time - previous_.time;
        if (dt > 0 && dt < 0.25) {
            const float drop = previous_.speed - s.speed;
            if (drop >= config_.impact_threshold) register_impact(drop);
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
        live_.duration = static_cast<float>(s.time - start_.time);
        if (s.speed < config_.settle_speed) {
            if (slow_since_ < 0) slow_since_ = s.time;
        } else {
            slow_since_ = -1;
        }
        const bool settled = slow_since_ >= 0 && s.time - slow_since_ >= config_.settle_time;
        const bool recovered = s.physics_state != config_.wipeout_state && live_.duration > 0.5f && s.speed < 2.0f;
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
