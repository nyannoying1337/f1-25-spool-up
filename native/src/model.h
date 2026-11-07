// model.h: the launch model and the F1 2026 test, free of game and Swofty types so they can be tested off-line.
// FIA-sourced: the 5 s Pre-Start and the 50 km/h MGU-K limit. Everything else is a tuning value, because the FIA has
// not published the detection thresholds of its start safety net or any turbo spool figure.
#pragma once

#include <cstdint>

namespace gs {

constexpr double PRE_START_S = 5.0;     // Section B, Article B5.7.2 b
constexpr float MGUK_MIN_KMH = 50.0f;   // Section C, ERS rules: "may only be used once the car has reached 50 km/h"

// What an F1 2026 car is, from the game's files. All 65 handling files of f1_2026_vehicle_package say HandlingVersion
// handling2026, which Car_Load_Vtf stores as 9 (year - 2017), and MaxDeployPower 350 kW. The 2025 and 2024 cars and every
// Classic Seasons car are 8 or 7 with 126.3 kW. F2 cars and the safety car have no ERS.
constexpr uint32_t F1_2026_HANDLING = 9;
constexpr float F1_2026_MIN_DEPLOY_W = 300e3f;

inline bool is_f1_2026(uint32_t handling_version, float max_deploy_w)
{
    return handling_version == F1_2026_HANDLING && max_deploy_w >= F1_2026_MIN_DEPLOY_W;
}

// A start is one rule for the whole grid: at least one F1 2026 car and no car with any other ERS. A car with no ERS at
// all is the safety car, which every grid has.
inline bool grid_is_f1_2026(uint32_t f1_2026_cars, uint32_t other_ers_cars)
{
    return f1_2026_cars > 0 && other_ers_cars == 0;
}

struct Tuning {
    float spool_s = 3.0f;       // seconds at spool revs from no boost to full boost
    float bog_power = 0.35f;    // power multiplier at zero boost: a missed spool is a real bog, about a third of the torque
    float decay_ratio = 2.0f;   // while waiting, boost falls this many times slower than it rises
    float detect_s = 1.0f;      // the safety net checks this long after the car started moving
    float min_accel = 4.5f;     // m/s^2, half a normal launch: below it the car counts as low-output, and the net holds it there
    float net_max_boost = 0.5f; // a car with this much boost has grip or driver trouble, not low power: wet starts stay unflagged
    float net_gain = 0.5f;      // share of the max deploy per m/s below the line
    float net_max_gap = 1.0f;   // m/s the line may run ahead of the car, so a lift is not paid back with a burst of MGU-K
    float min_moving_ms = 0.3f; // a car below this speed is parked, stalled or retired: the net ignores it
    float rate_per_s = 3.0f;    // power multiplier change per second, so 50 km/h is not a step
    float end_s = 12.0f;        // the launch phase ends for every car after this long
    float ai_fumble_hold_s = 1.5f; // a fumbled AI car's boost stays down this long after lights out
};

inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

// While the car waits for the lights: boost rises at spool revs and falls below them.
inline float spool_step(float boost, bool at_spool_revs, float dt, const Tuning &t)
{
    float rate = 1.f / (t.spool_s > 0.01f ? t.spool_s : 0.01f);
    return clamp01(at_spool_revs ? boost + rate * dt : boost - rate / t.decay_ratio * dt);
}

// After lights out: boost only rises. The revs always drop when the clutch bites, and that must not cost boost.
inline float launch_boost(float boost, bool at_spool_revs, float dt, const Tuning &t)
{
    return at_spool_revs ? spool_step(boost, true, dt, t) : boost;
}

// The engine multiplier: what the turbo gives. The game's own curve already keeps the MGU-K at 0 below 50 km/h.
inline float target_power(float boost, const Tuning &t)
{
    return t.bog_power + (1.f - t.bog_power) * clamp01(boost);
}

// The FIA's "low power start detection", "shortly after clutch release": checked once, in the first frame past detect_s
// after the car started moving, so a late reaction is not low output. gained_ms is the speed it gained in that time. A car
// with most of its boost is not low on power.
inline bool low_output(float speed_ms, float gained_ms, float moving_s, float boost, const Tuning &t)
{
    return moving_s >= t.detect_s && speed_ms >= t.min_moving_ms && gained_ms / moving_s < t.min_accel && boost < t.net_max_boost;
}

// The speed a flagged car is held to. It starts at the car's own speed when the net triggers, not at what a normal start
// has by then: that would hand back the speed it lost, and a worse start would end up level with a better one.
inline float net_line(float line_ms, float speed_ms, float dt, const Tuning &t)
{
    float next = line_ms + t.min_accel * dt, top = speed_ms + t.net_max_gap;
    return next < top ? next : top;
}

// Section C: "only to the extent necessary". The share of the max deploy the deploy curve gives a flagged car.
inline float mguk_share(float line_ms, float speed_ms, const Tuning &t) { return clamp01(t.net_gain * (line_ms - speed_ms)); }

// An AI car's boost at lights out, from two random numbers in 0..1. The real starts this follows: most clean, some poor, a
// few fumbled. The split is mine; "roughly 1 in 20 fumbled" is one driver's estimate.
constexpr float AI_FUMBLED_BOOST = 0.2f;

inline float ai_boost(float roll, float within)
{
    if (roll < 0.05f)
        return AI_FUMBLED_BOOST * within;
    if (roll < 0.25f)
        return AI_FUMBLED_BOOST + 0.3f * within;
    return 0.5f + 0.5f * within;
}

// When an AI car's boost starts to refill, in seconds after lights out. It refills at any revs, because the AI's rev pattern
// is not this mod's to judge, and that alone left a fumble barely a fumble: in the game log of 2026-11-07 a car with about
// 10 % at lights out had 49 % 1.2 s later and lost 0.5 s to the median, where the player's missed spool lost 1.65 s. So a
// fumbled car bogs first, as the player does.
inline float ai_refill_after(float boost, const Tuning &t) { return boost < AI_FUMBLED_BOOST ? t.ai_fumble_hold_s : 0.f; }

inline float rate_limit(float cur, float target, float dt, const Tuning &t)
{
    float max_step = t.rate_per_s * dt;
    float d = target - cur;
    return cur + (d > max_step ? max_step : d < -max_step ? -max_step : d);
}

} // namespace gs
