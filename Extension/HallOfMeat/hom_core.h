#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

// Hall Of Meat scoring core. Pure logic with no game or UI dependencies, so it can be unit tested:
// feed it one Sample per client tick and it tracks a bail from the moment the skater wipes out
// until they settle, detects impacts from sudden speed loss, breaks bones from those impacts and
// scores the whole bail. Scoring follows the Skate series: bones broken, distance, air time, with
// a multiplier for how much of the skeleton went.
namespace dingosdk::hall_of_meat {

struct Sample {
    double time{}; // seconds, monotonic
    std::array<float, 3> position{};
    float speed{};    // metres per second over the ground
    float vertical{}; // metres per second, upward positive
    bool airborne{};
    std::uint32_t physics_state{};
    float heading{}; // degrees, 0..360
    // Middle of each X-ray bone in world space (hom_rig.h order), when the skeleton could be read.
    bool bones_valid{};
    bool upright{}; // head well above the hips
    std::array<std::array<float, 3>, 19> bone_centres{};
};

struct Bone {
    const char *name;
    const char *region;
    int points;      // base score when broken
    float fragility; // 0..1, how readily it breaks
    float weight;    // how likely an impact is to land on it
};
const std::vector<Bone> &bones();

struct BrokenBone {
    std::size_t bone{};
    float impact{}; // m/s of speed lost in the hit that broke it
    double time{};  // when it broke (Sample::time)
};

struct Result {
    std::uint32_t serial{};
    std::vector<BrokenBone> broken;
    // Worst hit each bone took, as a share of what breaks it (1 = broken). The X-ray shows bones past ~0.5.
    std::array<float, 19> damage{};
    int impacts{};
    float distance{};    // metres from where the bail began
    float peak_speed{};  // m/s
    float peak_height{}; // metres above the start of the bail
    float air_time{};    // seconds off the ground during the bail
    float duration{};    // seconds
    float biggest_hit{}; // m/s
    float drop{};     // metres below where the bail began, at its lowest
    float rotation{}; // degrees turned about the vertical during the bail
    // Points per row of the original HUD: the bone chip on top, then the metrics.
    int bone_points{}, rotation_points{}, air_points{}, drop_points{}, duration_points{}, speed_points{};
    int total{};
    std::string title;
};

struct Config {
    std::uint32_t wipeout_state = 300; // physics state that starts a bail
    float impact_threshold = 3.5f;     // m/s lost between two ticks to count as a hit
    float bone_break_speed = 16.0f;     // m/s a bone's middle must lose at once to break (scaled by fragility)
    float settle_speed = 0.6f;         // below this for settle_time ends the bail
    float settle_time = 1.0f;
    float min_duration = 0.4f; // shorter wipeouts are not worth scoring
    float max_duration = 45.0f;
    // Calibrated against the original's HUD (video): air 6.4 s and drops past ~52 m both read 10,000;
    // a 7.10 s bail read 7,750 and 8.26 s read 11,333; 36 km/h read 785.
    float air_scale = 1850.0f; // points per second of air, capped at metric_cap
    float drop_scale = 190.0f; // points per metre dropped, capped
    float duration_scale = 3089.0f;
    float duration_start = 4.59f; // seconds before a bail starts paying
    float speed_scale = 21.8f;    // points per km/h of peak speed
    float rotation_scale = 5.0f;  // points per degree turned
    int metric_cap = 10000;
};

enum class Phase { idle, bailing, finished };

class Tracker {
public:
    explicit Tracker(Config config = {}) : config_(config) {}
    // Returns true on the tick a bail ends (result() is then valid).
    bool update(const Sample &sample);
    Phase phase() const { return phase_; }
    const Result &live() const { return live_; }   // the bail in progress, scored so far
    const Result &result() const { return last_; } // the last finished bail
    void reset();
    const Config &config() const { return config_; }

private:
    void score(Result &r) const;
    void register_impact(float drop);
    Config config_;
    Phase phase_{Phase::idle};
    Result live_{}, last_{};
    Sample previous_{}, start_{};
    bool have_previous_{};
    double slow_since_{-1}, air_start_{-1};
    std::uint32_t serial_{};
    std::uint64_t rng_{};
    std::vector<bool> is_broken_;
    std::array<std::array<float, 3>, 19> bone_velocity_{};
    bool have_bone_velocity_{};
    void break_bones_from_rig(const Sample &previous, const Sample &now, double dt);
};

const char *title_for(int total, std::size_t bones_broken);
} // namespace dingosdk::hall_of_meat
