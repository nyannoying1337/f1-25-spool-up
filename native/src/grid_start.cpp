// SPDX-License-Identifier: LicenseRef-Spool-Up-Attribution-1.0
// grid_start.cpp: Spool Up, FIA 2026 standing starts for F1 25 through the Swofty native API, on F1 2026 cars only.
//
// F1 2026 cars only. Before the wrap on the lights enter changes anything, it reads the handling version and the ERS max
// deploy of every car in the core's car table (model.h). Any other grid gets the game's own start: no Pre-Start, no run,
// no layers, nothing on screen.
//
// Pre-Start. SessionStateStartLights keeps a float countdown at state+0xdf0. Its update (F1_25.exe 0x142d8fcc0)
// subtracts dt while it is above 0 and starts the red light sequence when it reaches 0. Wrapping the enter and adding
// 5 s there delays the game's own sequence, so the lights, the AI reactions and the jump start rules are untouched. The
// update skips the countdown while the session is paused, so the Pre-Start pauses with the game.
//
// MGU-K. The handling file of every F1 2026 car has a deploy curve that is 0 up to 13.89 m/s, which is 50 km/h, and
// Car_Powertrain_Step caps the deploy at curve x max deploy for every car, AI included (0x1446e2a3d, 0x1446e2c30). That is
// the 50 km/h rule, and this mod leaves it alone on every car but one the safety net has flagged. For a flagged car below
// 50 km/h the wrap makes the curve answer the share the car needs (model.h), for the length of the game's call only:
// Car_Spline_EvalObfuscated (0x142e88290) answers the first sample at any speed when the last index is 0, so those two
// dwords are written and put back after the call. The curve is the car's own, inside its ERS record. Everything after the
// curve stays the game's: the mode's limit, the energy, the 500 Nm clamp, the throttle split and the brake and idle gates.
//
// Engine. An engine.power_scale layer (SwoftyCarOps) scales the drive torque for the turbo boost. Terminal Damage's fault scale (priority 50) and engine kill (set, 100) stay on top of this
// mod's scale (40). Layers are changed on a worker thread: on the frame's thread each change runs the core's whole
// apply pass for every car and field, which would be a hitch at lights out.
//
// Threads. Hooks only touch the game memory their own call was given, and hand values over through atomics. The cars at
// the lights enter come from the core's table and are read through ReadProcessMemory. So is the lights object from the
// frame, as it may already be gone. The frame never touches a car.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#include "model.h"
#include "swofty_api.h"

namespace {

constexpr char GS_VERSION[] = "0.2.1";

// Offsets, build d873a1c8d2044cef. CODE_CHECKS below turns the whole mod off on any other exe.
constexpr uintptr_t ST_SESSION = 0xc98;    // session: +0x1e8 state, +0x1a8 descriptor
constexpr uintptr_t ST_LIGHTS = 0xdb0;     // StartLights *
constexpr uintptr_t ST_ONCE = 0xdc8;       // u8, set by the enter's first run
constexpr uintptr_t ST_DELAY = 0xdf0;      // float, seconds until the lights sequence starts
constexpr uintptr_t SESSION_DESC = 0x1a8;  // descriptor: u8 kind, u32 session type at +4
constexpr uintptr_t SESSION_STATE = 0x1e8; // u32
constexpr uint32_t SESSION_PAUSED = 0x10;  // the lights update skips the countdown in this state
constexpr uintptr_t SL_MODE = 0x28;        // u8: 0 and 1 five lights, 2 the formation lap's one light
constexpr uint8_t SL_MODE_FORMATION = 2;
constexpr uintptr_t SL_STATE = 0x29;       // u8: 0 idle, 1 lights filling, 2 hold, 3 lights out, 4 off
constexpr uintptr_t SL_ENABLED = 0xbbd;    // u8: 0 means the game skips the lights
constexpr uintptr_t DYN_CAR = 0x18;        // CarDynamics, the argument of Car_Powertrain_Step
constexpr uintptr_t DYN_ENGINE = 0x80;
constexpr uintptr_t DYN_DT = 0xa48;        // float, seconds per substep, 0 when the tick has no time
constexpr uintptr_t CAR_VEHICLE_ID = 0xe8; // u8, slot in g_Sess_VehicleTable
constexpr uintptr_t CAR_IS_AI = 0x124;     // u32, 1 while the AI drives
constexpr uintptr_t CAR_CHASSIS = 0x558;
constexpr uintptr_t CAR_ERS = 0x5a8;
constexpr uintptr_t CAR_HANDLING = 0x9f40; // u32, from the car's handling file: gs::F1_2026_HANDLING
constexpr uintptr_t SYS_OWNER = 0x8;       // system+8 == its CarLive
constexpr uintptr_t ENGINE_RAD_S = 0x2538; // float
constexpr uintptr_t ERS_MAX_DEPLOY = 0x28; // float, W
constexpr uintptr_t ERS_DEPLOY = 0x1a0c;   // float, W, what this step deployed
constexpr uintptr_t ERS_CURVE_FLAG = 0x1a62; // u8, the step picks the second deploy curve when it is set
constexpr uintptr_t ERS_CURVE = 0x88;      // CarSpline, the deploy curve: m/s to a share of the max deploy
constexpr uintptr_t ERS_CURVE_2 = 0x330;   // CarSpline, the second one
constexpr uintptr_t SPLINE_SCALE = 0x60;   // CarObfFloat: the value is this dword ^ the dword OBF_KEY after it
constexpr uintptr_t SPLINE_OFFSET = 0x70;  // CarObfFloat
constexpr uintptr_t SPLINE_LAST = 0x90;    // int32, last sample index; below 0 the spline reads a point list instead
constexpr uintptr_t SPLINE_SAMPLES = 0x98; // CarObfFloat[], 16 bytes each, inside the spline
constexpr uintptr_t OBF_KEY = 0x8;
constexpr uintptr_t CH_RIGID = 0x340;      // physics -> rigid -> body, as the game's telemetry writer 0x142cf9820
constexpr uintptr_t RIGID_BODY = 0x18;
constexpr uintptr_t BODY_POS = 0x1c0;      // 3 floats, world, m
constexpr uintptr_t BODY_VEL = 0x220;      // 3 floats, world, m/s

struct CodeCheck {
    uintptr_t rva;
    const char *what;
    unsigned char bytes[18];
    size_t n;
};

// The instructions the offsets above were read from. None overlaps a hook this or the core installs.
const CodeCheck CODE_CHECKS[] = {
    {0x2d8fcc0, "lights update prologue", {0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x83, 0xEC, 0x50}, 14},
    {0x2cd5d80, "lights reset, enabled flag", {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x80, 0xB9, 0xBD, 0x0B, 0x00, 0x00, 0x00}, 18},
    {0x2d8fda4, "session pointer, pause state", {0x48, 0x8B, 0x87, 0x98, 0x0C, 0x00, 0x00, 0x83, 0xB8, 0xE8, 0x01, 0x00, 0x00, 0x10}, 14},
    {0x2d8fdb8, "the countdown", {0xC5, 0xFA, 0x10, 0x87, 0xF0, 0x0D, 0x00, 0x00}, 8},
    {0x2d8fdce, "lights object, state and mode", {0x48, 0x8B, 0x87, 0xB0, 0x0D, 0x00, 0x00, 0x80, 0x78, 0x29, 0x00, 0x74, 0x06, 0x80, 0x78, 0x28, 0x02}, 17},
    {0x2d904cd, "lights finished", {0x80, 0x78, 0x29, 0x04}, 4},
    {0x2d8faac, "first-run flag", {0xC6, 0x86, 0xC8, 0x0D, 0x00, 0x00, 0x01}, 7},
    {0x46e22f9, "powertrain car and engine", {0x48, 0x89, 0xCE, 0x48, 0x8B, 0x99, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x41, 0x18}, 14},
    {0x46e26ce, "engine speed", {0xC5, 0xFA, 0x10, 0x8B, 0x38, 0x25, 0x00, 0x00}, 8},
    {0x46e282d, "car ERS", {0x4C, 0x8B, 0xB0, 0xA8, 0x05, 0x00, 0x00}, 7},
    {0x46e2894, "max deploy, target", {0xC4, 0xC1, 0x7A, 0x10, 0x4E, 0x28}, 6},
    {0x46e29f1, "max deploy, cap", {0xC4, 0xC1, 0x7A, 0x10, 0x5E, 0x28}, 6},
    {0x46e2c87, "deploy written", {0xC4, 0xC1, 0x7A, 0x11, 0x86, 0x0C, 0x1A, 0x00, 0x00}, 9},
    {0x46dc53f, "substep time", {0xC5, 0xFA, 0x11, 0x96, 0x48, 0x0A, 0x00, 0x00}, 8},
    {0x2cf9a78, "body position", {0x48, 0x8B, 0x88, 0xC0, 0x01, 0x00, 0x00}, 7},
    {0x2cf9a87, "body velocity", {0x48, 0x8B, 0x90, 0x20, 0x02, 0x00, 0x00}, 7},
    {0x2cf9a45, "car chassis, its rigid body", {0x49, 0x8B, 0x95, 0x58, 0x05, 0x00, 0x00, 0x48, 0x8B, 0x92, 0x40, 0x03, 0x00, 0x00}, 14},
    {0x2cf9a5c, "the body", {0x48, 0x8B, 0x42, 0x18}, 4},
    {0x46e2366, "AI flag", {0x83, 0xB8, 0x24, 0x01, 0x00, 0x00, 0x01}, 7},
    {0x2d8fe19, "session descriptor", {0x48, 0x8B, 0x80, 0xA8, 0x01, 0x00, 0x00}, 7},
    {0x2c19601, "vehicle id", {0x41, 0x0F, 0xB6, 0x9E, 0xE8, 0x00, 0x00, 0x00}, 8},
    {0x46e289a, "system owner", {0x49, 0x8B, 0x4E, 0x08}, 4},
    {0x46e29e0, "handling version, against 2026's", {0x83, 0xB8, 0x40, 0x9F, 0x00, 0x00, 0x09}, 7},
    {0x2e88290, "curve last index", {0x8B, 0x81, 0x90, 0x00, 0x00, 0x00, 0x85, 0xC0, 0x78, 0x77}, 10},
    {0x2e8829a, "curve scale and offset", {0x8B, 0x51, 0x60, 0x44, 0x8B, 0x41, 0x70, 0x33, 0x51, 0x68}, 10},
    {0x2e882a8, "curve offset key", {0x44, 0x33, 0x41, 0x78}, 4},
    {0x2e882bd, "curve at or below the first sample", {0xC5, 0xF8, 0x2E, 0xC8, 0x0F, 0x83, 0xA3, 0x00, 0x00, 0x00}, 10},
    {0x2e882cc, "curve at or past the last index", {0x39, 0xD0, 0x0F, 0x8E, 0xA7, 0x00, 0x00, 0x00}, 8},
    {0x2e8836a, "curve first sample", {0x8B, 0x81, 0x98, 0x00, 0x00, 0x00, 0x33, 0x81, 0xA0, 0x00, 0x00, 0x00}, 12},
    {0x2e8837f, "curve last sample", {0x8B, 0x94, 0x01, 0x98, 0x00, 0x00, 0x00, 0x33, 0x94, 0x01, 0xA0, 0x00, 0x00, 0x00}, 14},
    {0x466db1b, "the loader's scale and offset of 0", {0x44, 0x8B, 0x4E, 0x68, 0x44, 0x89, 0x4E, 0x60, 0x44, 0x8B, 0x46, 0x78, 0x44, 0x89, 0x46, 0x70}, 16},
    {0x46e2979, "deploy curve, first and second", {0x49, 0x8D, 0x46, 0x70, 0x49, 0x8D, 0x8E, 0x18, 0x03, 0x00, 0x00}, 11},
    {0x46e2984, "deploy curve flag", {0x41, 0x80, 0xBE, 0x62, 0x1A, 0x00, 0x00, 0x00}, 8},
    {0x46e29cd, "deploy curve call", {0x48, 0x83, 0xC1, 0x18, 0xC5, 0xF8, 0x28, 0x4C, 0x24, 0x40, 0xE8, 0xB4, 0x58, 0x7A, 0xFE}, 15},
    {0x46e2a3d, "cap: curve x max deploy", {0xC5, 0xFA, 0x59, 0xF3}, 4},
};

constexpr uint32_t MAX_CARS = 26;
constexpr uint32_t SAMPLE_EVERY = 6;      // of the 360 substeps a second: 60 samples per car
constexpr float RAD_S_PER_RPM = 0.10472f;
constexpr double LIGHTS_TIMEOUT_S = 40.0;
constexpr float WARN_RANGE_M = 120.f;
constexpr float NET_FLASH_S = 5.f;
constexpr int32_t LAYER_PRIORITY = 40;
constexpr float LAYER_STEP = 0.05f;       // a layer is only replaced when the power moved this much
constexpr DWORD LAYER_PAUSE_MS = 40;      // the core reapplies its layers once per frame with a change: at most 25 a second
constexpr uint64_t FROZEN_MS = 250;       // no physics time for this long: the game is frozen
constexpr char LAYER_OWNER[] = "nick.grid-start-2026";

const SwoftyApi *g_api;
const SwoftyCarOps *g_cars;
const SwoftyOverlay *g_ov;
const SwoftyHooks *g_hooks;
uint32_t g_ov_ver = 1, g_ov_token, g_power_field;
double g_ov_retry;

using EnterFn = uintptr_t (*)(void *, void *, void *, void *);
using PowertrainFn = void (*)(void *);
EnterFn g_enter_orig;
PowertrainFn g_powertrain_orig;

template <class... A> void glog(const char *fmt, A... a)
{
    if (!g_api || !g_api->log)
        return;
    char b[700];
    if constexpr (sizeof...(A) == 0)
        snprintf(b, sizeof b, "%s", fmt);
    else
        snprintf(b, sizeof b, fmt, a...);
    g_api->log("[gridstart] %s", b);
}

// For memory the game's own call was just given: it reads the same pointers next.
template <class T> bool rd(uintptr_t at, T &out)
{
    __try {
        out = *reinterpret_cast<const T *>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// For a kept pointer: never faults, so the core's crash handler stays quiet when the object is gone.
template <class T> bool guarded_read(uintptr_t at, T &out)
{
    T v{};
    SIZE_T got = 0;
    if (!at || !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(at), &v, sizeof v, &got) || got != sizeof v)
        return false;
    out = v;
    return true;
}

double now_s()
{
    static const double freq = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return double(v.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / freq;
}

constexpr std::memory_order RELAXED = std::memory_order_relaxed;

// Settings
std::atomic<bool> s_pre_start{true}, s_launch{true}, s_net{true}, s_ai{true};
std::atomic<float> s_spool{3.0f}, s_spool_rpm{10000.f};

gs::Tuning tuning()
{
    gs::Tuning t;
    t.spool_s = s_spool;
    return t;
}

void read_settings()
{
    if (!g_api || !g_api->setting)
        return;
    SwoftySettingValue v{};
    auto tog = [&](const char *id, std::atomic<bool> &o) {
        if (g_api->setting(id, SWOFTY_SETTING_TOGGLE, &v))
            o = v.toggle != 0;
    };
    auto sli = [&](const char *id, std::atomic<float> &o, float k) {
        if (g_api->setting(id, SWOFTY_SETTING_SLIDER, &v))
            o = float(v.slider) * k;
    };
    tog("pre_start", s_pre_start);
    tog("launch_model", s_launch);
    tog("safety_net", s_net);
    tog("apply_to_ai", s_ai);
    sli("spool_seconds", s_spool, 1.f);
    sli("spool_rpm", s_spool_rpm, 1.f);
}

// ------------------------------------------------------------------------------------------------
// State

enum Phase : int { P_NONE, P_PRE, P_LIGHTS, P_LAUNCH };
std::atomic<int> g_phase{P_NONE};

// One writer per car (the game steps a car on one thread at a time); read by the frame.
struct LiveCar {
    std::atomic<uint32_t> steps{0};
    std::atomic<double> sim_s{0};
    std::atomic<float> rad_s{0}, peak_rad_s{0}, speed{0}, px{0}, pz{0}, vx{0}, vz{0};
    std::atomic<float> max_deploy{0}, deploy_below{0}, deploy_above{0};
    std::atomic<float> assist{0}; // the frame asks, the hook applies: share of the max deploy below 50 km/h, for a flagged car
    std::atomic<bool> seen{false}, ai{true}, reached_50{false};
};
LiveCar g_live[MAX_CARS];
std::atomic<uint64_t> g_tick_ms{0}; // when physics time last moved

// Hooks and events to the frame
std::atomic<uintptr_t> g_pending_st{0};
std::atomic<float> g_pending_delay{0};
std::atomic<bool> g_pending_pre{false};
std::atomic<const char *> g_end_request{nullptr};
std::atomic<bool> g_armed{false}; // session.start said this is an offline race

// Frame to overlay
std::atomic<float> g_player_boost{0}, g_delay_left{0};
std::atomic<bool> g_player_flag{false}, g_ahead_flag{false}, g_frozen{false};

// Frame only
struct CarRun {
    float boost = 0, power = 1, posted = 1, flag_until = 0, px = 0, pz = 0, reached_s = -1;
    float moved_s = -1, moved_speed = 0, net_line = 0; // when it started moving, its speed then; the speed the net holds it to
    float hold_s = 0; // a fumbled AI car's: its boost stays where it is this long after lights out
    bool active = false, checked = false, flagged = false, done = false, ai = false;
};
CarRun g_run[MAX_CARS];
uintptr_t g_state, g_state_vt;
double g_sim_prev, g_wait_s, g_wait_sim_s, g_worst_wait, g_worst_launch;
float g_launch_s, g_delay_prev;
uint64_t g_delay_ms;
int g_player = -1;
bool g_data_logged, g_layers_dirty;
std::mt19937 g_rng{std::random_device{}()};

// ------------------------------------------------------------------------------------------------
// Layers, on their own thread

std::atomic<float> g_want[MAX_CARS]; // read as 1 when no layer is wanted
std::atomic<uint64_t> g_layer_posts{0}, g_layer_done{0};
uint32_t g_token[MAX_CARS];          // the worker's
float g_token_value[MAX_CARS];       // the worker's
bool g_layer_logged;                 // the worker's
HANDLE g_layer_event;

void apply_layers()
{
    for (uint32_t i = 0; i < MAX_CARS; i++) {
        float v = g_want[i].load(RELAXED);
        if (v >= 0.999f) {
            if (g_token[i]) {
                g_cars->unlayer(g_token[i]);
                g_token[i] = 0;
            }
            continue;
        }
        if (g_token[i] && v == g_token_value[i])
            continue;
        uint32_t tok = 0;
        int r = g_cars->layer(LAYER_OWNER, i, g_power_field, SWOFTY_CAR_OP_SCALE, v, LAYER_PRIORITY, &tok);
        if (r != SWOFTY_ASSET_OK) {
            if (!g_layer_logged) {
                g_layer_logged = true;
                glog("no engine.power_scale layer (result %d): the launch keeps normal engine power", r);
            }
            continue;
        }
        if (g_token[i])
            g_cars->unlayer(g_token[i]); // after the new one, so the car never runs at full power in between
        g_token[i] = tok;
        g_token_value[i] = v;
        if (!g_layer_logged) {
            g_layer_logged = true;
            float now = -1.f;
            g_cars->get(i, nullptr, g_power_field, &now);
            glog("engine.power_scale layer on car %u, now %.2f (asked %.2f)", i, now, v);
        }
    }
}

void apply_layers_guarded()
{
    __try {
        apply_layers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        glog("a layer change faulted");
    }
}

DWORD WINAPI layer_worker(void *)
{
    for (;;) {
        WaitForSingleObject(g_layer_event, INFINITE);
        uint64_t posts = g_layer_posts.load();
        apply_layers_guarded();
        g_layer_done.store(posts);
        Sleep(LAYER_PAUSE_MS);
    }
}

void start_layer_worker()
{
    g_layer_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE th = g_layer_event ? CreateThread(nullptr, 0, layer_worker, nullptr, 0, nullptr) : nullptr;
    if (th)
        CloseHandle(th);
    else
        g_layer_event = nullptr;
}

void flush_layers()
{
    if (!g_layers_dirty || !g_cars)
        return;
    g_layers_dirty = false;
    g_layer_posts.fetch_add(1);
    if (g_layer_event)
        SetEvent(g_layer_event);
    else
        apply_layers_guarded(); // no worker thread: slower, still correct
}

void want_power(uint32_t i, CarRun &c, float v)
{
    float next = v >= 0.999f ? 1.f : v;
    if (next == 1.f ? c.posted == 1.f : std::fabs(next - c.posted) < LAYER_STEP)
        return;
    c.posted = next;
    g_want[i].store(next, RELAXED);
    g_layers_dirty = true;
}

// ------------------------------------------------------------------------------------------------
// Hooks

struct GridRead {
    uint32_t cars = 0, f1_2026 = 0, other = 0, handling = 0;
    float deploy = 0; // of the last car with an ERS, for the log
};

GridRead read_grid()
{
    GridRead g;
    uint32_t n = 0;
    if (!g_cars || !g_cars->count || !g_cars->car || g_cars->count(&n) != SWOFTY_ASSET_OK)
        return g;
    for (uint32_t i = 0; i < n && i < MAX_CARS; i++) {
        void *handle = nullptr;
        if (g_cars->car(i, &handle, nullptr) != SWOFTY_ASSET_OK || !handle)
            continue;
        g.cars++;
        auto car = reinterpret_cast<uintptr_t>(handle);
        uintptr_t ers = 0, owner = 0;
        uint32_t handling = 0;
        float deploy = 0;
        if (!guarded_read(car + CAR_ERS, ers) || !ers)
            continue; // no ERS: the safety car
        if (!guarded_read(ers + SYS_OWNER, owner) || owner != car || !guarded_read(ers + ERS_MAX_DEPLOY, deploy) || !guarded_read(car + CAR_HANDLING, handling))
            continue;
        if (gs::is_f1_2026(handling, deploy))
            g.f1_2026++;
        else
            g.other++;
        g.handling = handling;
        g.deploy = deploy;
    }
    return g;
}

void on_enter(uintptr_t st, uint8_t once)
{
    uintptr_t sl = 0;
    uint8_t lights = 0xff, mode = 0, enabled = 0;
    float delay = 0;
    if (!rd(st + ST_LIGHTS, sl) || !sl || !rd(sl + SL_STATE, lights) || !rd(sl + SL_MODE, mode) || !rd(sl + SL_ENABLED, enabled) ||
        !rd(st + ST_DELAY, delay)) {
        glog("StartLights enter: the state is unreadable, standard start");
        return;
    }
    // The update starts a sequence only from these two states: otherwise this is a start resuming, or lights already over.
    if (lights != 0 && mode != SL_MODE_FORMATION)
        return;
    // The game sets the delay once per state object. A later enter on the same object is a new start only when no run
    // waits for the lights and no countdown is left (a race restart).
    int ph = g_phase.load();
    bool waiting = g_pending_st.load() != 0 || ph == P_PRE || ph == P_LIGHTS;
    if (once != 0 && (delay > 0.f || waiting))
        return;

    GridRead grid = read_grid();
    glog("StartLights enter: %u cars, %u F1 2026, %u others with an ERS (handling version %u, max deploy %.0f W)", grid.cars, grid.f1_2026, grid.other, grid.handling, grid.deploy);

    // Same descriptor as Game_CrowdSystem_ApplySessionMood: 1-4 practice, 5-14 qualifying, 15-17 race, 18 time trial.
    uint8_t kind = 0xff;
    uint32_t type = 0;
    uintptr_t session = 0, desc = 0;
    if (rd(st + ST_SESSION, session) && session && rd(session + SESSION_DESC, desc) && desc) {
        rd(desc, kind);
        rd(desc + 4, type);
    }
    if (!g_armed.load()) {
        glog("StartLights enter: standard start, not an offline race (kind %u, type %u)", kind, type);
        return;
    }
    if (type >= 1 && type <= 18 && (type < 15 || type > 17)) {
        glog("StartLights enter: standard start, session type %u is not a race (kind %u)", type, kind);
        return;
    }
    if (!enabled) {
        glog("StartLights enter: standard start, the game skips the lights here");
        return;
    }
    if (!gs::grid_is_f1_2026(grid.f1_2026, grid.other)) {
        glog("StartLights enter: standard start, not F1 2026 cars");
        return;
    }
    bool pre = s_pre_start.load();
    if (!pre && !s_launch.load())
        return;
    // After a formation lap the lights object still says "finished" from the formation's one light. The game restarts
    // it in its first update, but during a countdown the update would read that as lights over and skip the whole start.
    if (lights != 0)
        *reinterpret_cast<uint8_t *>(sl + SL_STATE) = 0;
    if (pre)
        *reinterpret_cast<float *>(st + ST_DELAY) = (delay > 0.f ? delay : 0.f) + float(gs::PRE_START_S);
    g_pending_delay = delay;
    g_pending_pre = pre;
    g_pending_st = st; // last: the frame takes the run from here
    glog("StartLights enter: session kind %u, type %u, first run %d", kind, type, once == 0);
}

void on_enter_guarded(uintptr_t st, uint8_t once)
{
    __try {
        on_enter(st, once);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uintptr_t enter_hook(void *self, void *b, void *c, void *d)
{
    uint8_t once = 1;
    auto st = reinterpret_cast<uintptr_t>(self);
    rd(st + ST_ONCE, once);
    uintptr_t r = g_enter_orig(self, b, c, d);
    on_enter_guarded(st, once);
    return r;
}

void sample_car(uintptr_t pt, uintptr_t car, uintptr_t ers, LiveCar &l)
{
    uintptr_t engine = *reinterpret_cast<uintptr_t *>(pt + DYN_ENGINE);
    if (engine && *reinterpret_cast<uintptr_t *>(engine + SYS_OWNER) == car) {
        float rad = *reinterpret_cast<float *>(engine + ENGINE_RAD_S);
        if (std::isfinite(rad) && rad >= 0.f) {
            l.rad_s.store(rad, RELAXED);
            if (rad > l.peak_rad_s.load(RELAXED))
                l.peak_rad_s.store(rad, RELAXED);
        }
    }
    l.ai.store(*reinterpret_cast<uint32_t *>(car + CAR_IS_AI) == 1, RELAXED);
    if (ers && l.max_deploy.load(RELAXED) <= 0.f && *reinterpret_cast<uintptr_t *>(ers + SYS_OWNER) == car) {
        float md = *reinterpret_cast<float *>(ers + ERS_MAX_DEPLOY);
        if (std::isfinite(md) && md > 0.f)
            l.max_deploy.store(md, RELAXED);
    }
    uintptr_t physics = *reinterpret_cast<uintptr_t *>(car + CAR_CHASSIS);
    uintptr_t rigid = physics ? *reinterpret_cast<uintptr_t *>(physics + CH_RIGID) : 0;
    uintptr_t body = rigid ? *reinterpret_cast<uintptr_t *>(rigid + RIGID_BODY) : 0;
    if (!body)
        return;
    auto *vel = reinterpret_cast<const float *>(body + BODY_VEL);
    auto *pos = reinterpret_cast<const float *>(body + BODY_POS);
    float speed = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    if (!std::isfinite(speed) || speed > 300.f)
        return;
    l.speed.store(speed, RELAXED);
    l.vx.store(vel[0], RELAXED);
    l.vz.store(vel[2], RELAXED);
    l.px.store(pos[0], RELAXED);
    l.pz.store(pos[2], RELAXED);
    // "Once the car has reached 50 km/h": it stays on after that. Only from lights out: a car still rolling into its
    // grid slot has not started.
    if (speed * 3.6f >= gs::MGUK_MIN_KMH && g_phase.load(RELAXED) == P_LAUNCH)
        l.reached_50.store(true, RELAXED);
    l.seen.store(true, RELAXED);
}

// What the step's call needs put back and read back.
struct StepWatch {
    uintptr_t ers = 0;
    LiveCar *live = nullptr;
    uintptr_t curve = 0;
    int32_t saved_last = 0;
    uint32_t saved_sample = 0;
};

// The car's deploy curve answers the share at any speed, until step_end. With the last index at 0 the game's code reads the
// first sample whichever way the index goes (0x142e8836a, 0x142e8837f): the loader leaves a point list's scale and offset
// at 0 (0x14466db1b), and a table's are its real ones, so the index stays small. Any reader on another thread gets a number
// from inside the record, whichever of these two writes it catches.
void curve_override(uintptr_t ers, float share, StepWatch &w)
{
    uintptr_t curve = ers + (*reinterpret_cast<uint8_t *>(ers + ERS_CURVE_FLAG) ? ERS_CURVE_2 : ERS_CURVE);
    auto *last = reinterpret_cast<volatile int32_t *>(curve + SPLINE_LAST);
    auto *sample = reinterpret_cast<volatile uint32_t *>(curve + SPLINE_SAMPLES);
    w.saved_last = *last;
    w.saved_sample = *sample;
    w.curve = curve;
    uint32_t bits;
    memcpy(&bits, &share, sizeof bits);
    *sample = bits ^ *reinterpret_cast<const uint32_t *>(curve + SPLINE_SAMPLES + OBF_KEY);
    *last = 0;
}

void step_begin(uintptr_t pt, StepWatch &w)
{
    __try {
        uintptr_t car = *reinterpret_cast<uintptr_t *>(pt + DYN_CAR);
        if (!car)
            return;
        uint8_t id = *reinterpret_cast<uint8_t *>(car + CAR_VEHICLE_ID);
        if (id >= MAX_CARS)
            return;
        LiveCar &l = g_live[id];
        float dt = *reinterpret_cast<float *>(pt + DYN_DT);
        uintptr_t ers = *reinterpret_cast<uintptr_t *>(car + CAR_ERS);
        uint32_t n = l.steps.load(RELAXED);
        l.steps.store(n + 1, RELAXED);
        bool sample = n % SAMPLE_EVERY == 0;
        if (dt > 0.f && dt < 0.25f) {
            l.sim_s.store(l.sim_s.load(RELAXED) + dt, RELAXED);
            if (sample)
                g_tick_ms.store(GetTickCount64(), RELAXED);
        }
        // Every step, or the game's 0 comes back in between. This is a write, so it checks the record is the car's own.
        float assist = l.assist.load(RELAXED);
        if (assist > 0.f && ers && !l.reached_50.load(RELAXED) && *reinterpret_cast<uintptr_t *>(ers + SYS_OWNER) == car)
            curve_override(ers, assist, w);
        if (!sample)
            return;
        sample_car(pt, car, ers, l);
        if (ers && (!l.ai.load(RELAXED) || assist > 0.f)) {
            w.ers = ers;
            w.live = &l;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void step_end(const StepWatch &w)
{
    if (!w.curve && !w.ers)
        return;
    __try {
        if (w.curve) {
            *reinterpret_cast<volatile int32_t *>(w.curve + SPLINE_LAST) = w.saved_last;
            *reinterpret_cast<volatile uint32_t *>(w.curve + SPLINE_SAMPLES) = w.saved_sample;
        }
        if (w.ers) {
            float d = *reinterpret_cast<float *>(w.ers + ERS_DEPLOY);
            auto &peak = w.live->reached_50.load(RELAXED) ? w.live->deploy_above : w.live->deploy_below;
            if (d > peak.load(RELAXED))
                peak.store(d, RELAXED);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void powertrain_hook(void *pt)
{
    if (g_phase.load(RELAXED) == P_NONE) {
        g_powertrain_orig(pt);
        return;
    }
    StepWatch w;
    step_begin(reinterpret_cast<uintptr_t>(pt), w);
    g_powertrain_orig(pt);
    step_end(w);
}

// ------------------------------------------------------------------------------------------------
// Frame

void reset_live()
{
    for (auto &l : g_live) {
        l.steps = 0;
        l.sim_s = 0.0;
        l.rad_s = 0;
        l.peak_rad_s = 0;
        l.speed = 0;
        l.px = 0;
        l.pz = 0;
        l.vx = 0;
        l.vz = 0;
        l.max_deploy = 0;
        l.deploy_below = 0;
        l.deploy_above = 0;
        l.assist = 0;
        l.seen = false;
        l.ai = true;
        l.reached_50 = false;
    }
}

void end_run(const char *why)
{
    bool launched = g_phase.load() == P_LAUNCH;
    g_phase = P_NONE;
    for (uint32_t i = 0; i < MAX_CARS; i++)
        want_power(i, g_run[i], 1.f);
    flush_layers();
    g_state = 0;
    g_player_flag = false;
    g_ahead_flag = false;
    g_frozen = false;
    if (launched)
        glog("launch power restored on all cars (%s)", why);
    else
        glog("run over before lights out (%s)", why);
}

void start_run(uintptr_t st)
{
    if (g_phase.load() != P_NONE)
        end_run("a new start");
    reset_live();
    for (auto &c : g_run)
        c = CarRun{};
    g_state = st;
    g_state_vt = 0;
    guarded_read(st, g_state_vt);
    g_sim_prev = 0;
    g_wait_s = 0;
    g_wait_sim_s = 0;
    g_worst_wait = 0;
    g_worst_launch = 0;
    g_launch_s = 0;
    g_delay_prev = -1.f;
    g_delay_ms = GetTickCount64();
    g_tick_ms = GetTickCount64();
    g_player = -1;
    g_player_boost = 0;
    g_data_logged = false;
    g_frozen = false;
    float delay = g_pending_delay;
    bool pre = g_pending_pre;
    g_delay_left = delay + (pre ? float(gs::PRE_START_S) : 0.f);
    glog("run started: game delay %.2f s, Pre-Start %s, launch model %s, safety net %s, AI %s, spool %.1f s at %.0f rpm", delay, pre ? "+5.00 s" : "off",
         s_launch.load() ? "on" : "off", s_net.load() ? "on" : "off", s_ai.load() ? "on" : "off", s_spool.load(), s_spool_rpm.load());
    g_phase = pre ? P_PRE : P_LIGHTS;
}

void log_summary()
{
    int flagged = 0, ai = 0, ai_reached = 0;
    float ai_times[MAX_CARS];
    for (uint32_t i = 0; i < MAX_CARS; i++) {
        const CarRun &c = g_run[i];
        flagged += c.flagged;
        if (c.flagged)
            glog("launch done: low-output car %u: 50 km/h after %.2f s, MGU-K peak %.0f W below 50 km/h", i, c.reached_s, g_live[i].deploy_below.load());
        if (c.hold_s > 0.f)
            glog("launch done: fumbled AI car %u: 50 km/h after %.2f s", i, c.reached_s);
        if (!c.active || !c.ai)
            continue;
        ai++;
        if (c.reached_s >= 0.f)
            ai_times[ai_reached++] = c.reached_s;
    }
    if (s_ai.load()) {
        std::sort(ai_times, ai_times + ai_reached);
        if (ai_reached)
            glog("launch done: AI cars at 50 km/h: %d of %d, first %.2f s, median %.2f s, the last after %.2f s", ai_reached, ai, ai_times[0], ai_times[ai_reached / 2], ai_times[ai_reached - 1]);
        else
            glog("launch done: AI cars at 50 km/h: 0 of %d", ai);
    }
    glog("launch done: worst frame %.0f ms in the launch, %.0f ms while waiting", g_worst_launch * 1000.0, g_worst_wait * 1000.0);
    if (g_player >= 0) {
        const LiveCar &l = g_live[g_player];
        glog("launch done: %d low-output car(s); player car %d: 50 km/h after %.2f s, MGU-K peak %.0f W below 50 km/h, %.0f W above",
             flagged, g_player, g_run[g_player].reached_s, l.deploy_below.load(), l.deploy_above.load());
    } else {
        glog("launch done: %d low-output car(s); no player car", flagged);
    }
}

// Every car's boost, while the cars wait for the lights.
void spool_cars(float dt, const gs::Tuning &t)
{
    float rad = s_spool_rpm.load() * RAD_S_PER_RPM;
    for (uint32_t i = 0; i < MAX_CARS; i++) {
        const LiveCar &l = g_live[i];
        if (!l.seen.load(RELAXED))
            continue;
        CarRun &c = g_run[i];
        c.boost = gs::spool_step(c.boost, l.rad_s.load(RELAXED) >= rad, dt, t);
        if (!l.ai.load(RELAXED)) {
            g_player = int(i);
            g_player_boost = c.boost;
        }
    }
}

void begin_launch(const gs::Tuning &t)
{
    int seen = 0;
    for (const auto &l : g_live)
        seen += l.seen.load(RELAXED);
    float peak = g_player >= 0 ? g_live[g_player].peak_rad_s.load() / RAD_S_PER_RPM : 0.f;
    glog("LIGHTS OUT after %.2f s of waiting (%.2f s of physics time); %d cars seen, player car %d, boost %.0f%%, peak %.0f rpm",
         g_wait_s, g_wait_sim_s, seen, g_player, g_player_boost.load() * 100.f, peak);
    if (!s_launch.load()) {
        end_run("launch model off");
        return;
    }
    if (!g_cars || !g_power_field) {
        end_run("no engine.power_scale");
        return;
    }
    if (!seen) {
        end_run("no engine data while waiting");
        return;
    }
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    bool any = false;
    int ai_good = 0, ai_poor = 0, ai_fumbled = 0;
    for (uint32_t i = 0; i < MAX_CARS; i++) {
        const LiveCar &l = g_live[i];
        CarRun &c = g_run[i];
        float md = l.max_deploy.load(RELAXED);
        if (!l.seen.load(RELAXED) || md < gs::F1_2026_MIN_DEPLOY_W)
            continue; // not on the grid, or no 2026 power unit (the safety car has no ERS): the rule is not for it
        c.active = true;
        c.ai = l.ai.load(RELAXED);
        if (c.ai && !s_ai.load()) {
            c.done = true;
            continue;
        }
        if (c.ai) {
            float roll = unit(g_rng);
            c.boost = gs::ai_boost(roll, unit(g_rng));
            c.hold_s = gs::ai_refill_after(c.boost, t);
            ai_fumbled += c.boost < gs::AI_FUMBLED_BOOST;
            ai_poor += c.boost >= gs::AI_FUMBLED_BOOST && c.boost < 0.5f;
            ai_good += c.boost >= 0.5f;
        }
        c.power = gs::target_power(c.boost, t);
        any = true;
        want_power(i, c, c.power);
    }
    if (!any) {
        end_run("no F1 2026 car on the grid");
        return;
    }
    if (s_ai.load())
        glog("AI spool at lights out: %d good, %d poor, %d fumbled", ai_good, ai_poor, ai_fumbled);
    g_launch_s = 0;
    g_phase = P_LAUNCH;
    flush_layers();
}

void launch_frame(float dt, const gs::Tuning &t)
{
    g_launch_s += dt;
    float since = g_launch_s;
    if (since > t.end_s) {
        log_summary();
        end_run("launch window over");
        return;
    }
    float rad = s_spool_rpm.load() * RAD_S_PER_RPM;
    bool all_done = true;
    float me_x = 0, me_z = 0, me_vx = 0, me_vz = 0, me_speed = 0;
    bool have_me = false;
    for (uint32_t i = 0; i < MAX_CARS; i++) {
        CarRun &c = g_run[i];
        if (!c.active)
            continue;
        LiveCar &l = g_live[i];
        float speed = l.speed.load(RELAXED);
        c.px = l.px.load(RELAXED);
        c.pz = l.pz.load(RELAXED);
        bool me = int(i) == g_player;
        if (me) {
            me_x = c.px;
            me_z = c.pz;
            me_vx = l.vx.load(RELAXED);
            me_vz = l.vz.load(RELAXED);
            me_speed = speed;
            have_me = true;
        }
        if (c.done)
            continue;
        bool reached = l.reached_50.load(RELAXED);
        if (reached && c.reached_s < 0.f)
            c.reached_s = since;
        // AI boost rises regardless of revs, the AI's rev pattern is not this mod's to judge, but a fumbled car bogs first.
        bool refills = c.ai ? since >= c.hold_s : l.rad_s.load(RELAXED) >= rad;
        c.boost = gs::launch_boost(c.boost, refills, dt, t);
        if (c.moved_s < 0.f && speed >= t.min_moving_ms) {
            c.moved_s = since;
            c.moved_speed = speed;
        }
        float moving = c.moved_s < 0.f ? 0.f : since - c.moved_s;
        if (s_net.load() && !c.checked && moving >= t.detect_s) {
            c.checked = true;
            if (!reached && gs::low_output(speed, speed - c.moved_speed, moving, c.boost, t)) {
                c.flagged = true;
                c.flag_until = since + NET_FLASH_S;
                c.net_line = speed;
                glog("low-output start: car %u, %.1f km/h %.2f s after it moved, boost %.0f%%: MGU-K on", i, speed * 3.6f, moving, c.boost * 100.f);
            }
        }
        float assist = 0.f;
        if (c.flagged && !reached) {
            c.net_line = gs::net_line(c.net_line, speed, dt, t);
            if (speed >= t.min_moving_ms) // a car that stopped again is parked, as far as the net goes
                assist = gs::mguk_share(c.net_line, speed, t);
        }
        l.assist.store(assist, RELAXED);
        c.power = gs::rate_limit(c.power, gs::target_power(c.boost, t), dt, t);
        if (reached && c.boost >= 1.f && c.power >= 0.999f) {
            c.done = true;
            want_power(i, c, 1.f);
        } else {
            want_power(i, c, c.power);
            all_done = false;
        }
    }
    flush_layers();
    // F1 25 has no known call for the rear and lateral lights, so a flagged car ahead shows on the overlay instead.
    bool ahead = false;
    if (have_me && me_speed > 2.f)
        for (uint32_t i = 0; i < MAX_CARS; i++) {
            const CarRun &c = g_run[i];
            if (int(i) == g_player || !c.flagged || since > c.flag_until)
                continue;
            float dx = c.px - me_x, dz = c.pz - me_z;
            if (dx * dx + dz * dz < WARN_RANGE_M * WARN_RANGE_M && dx * me_vx + dz * me_vz > 0.f)
                ahead = true;
        }
    g_ahead_flag = ahead;
    g_player_flag = g_player >= 0 && g_run[g_player].flagged && since <= g_run[g_player].flag_until;
    if (g_player >= 0)
        g_player_boost = g_run[g_player].boost;
    if (all_done) {
        log_summary();
        end_run("all cars past 50 km/h with full boost");
    }
}

void wait_frame(float sim_dt, double frame_dt, const gs::Tuning &t)
{
    float delay = 0;
    uintptr_t vt = 0, sl = 0, session = 0;
    uint8_t lights = 0;
    uint32_t session_state = 0;
    if (!guarded_read(g_state, vt) || vt != g_state_vt || !guarded_read(g_state + ST_DELAY, delay) ||
        !guarded_read(g_state + ST_LIGHTS, sl) || !sl || !guarded_read(sl + SL_STATE, lights)) {
        end_run("the lights state is gone");
        return;
    }
    if (guarded_read(g_state + ST_SESSION, session))
        guarded_read(session + SESSION_STATE, session_state);
    uint64_t ms = GetTickCount64();
    if (delay != g_delay_prev) {
        g_delay_prev = delay;
        g_delay_ms = ms;
    }
    // Paused: the state the lights update itself checks. The second test covers a freeze that state does not show.
    bool frozen = session_state == SESSION_PAUSED ||
                  (ms - g_tick_ms.load(RELAXED) > FROZEN_MS && ms - g_delay_ms > FROZEN_MS);
    g_frozen = frozen;
    g_delay_left = delay;
    if (!frozen)
        g_wait_s += frame_dt;
    g_wait_sim_s += sim_dt;
    spool_cars(sim_dt, t);
    if (!g_data_logged && g_player >= 0) {
        g_data_logged = true;
        const LiveCar &l = g_live[g_player];
        glog("engine data: player car %d at %.0f rpm, max deploy %.0f W", g_player, l.rad_s.load() / RAD_S_PER_RPM, l.max_deploy.load());
    }
    if (g_phase.load() == P_PRE && (lights != 0 || delay <= 0.f)) {
        float peak = g_player >= 0 ? g_live[g_player].peak_rad_s.load() / RAD_S_PER_RPM : 0.f;
        glog("Pre-Start over after %.2f s (lights state %u); player boost %.0f%%, peak %.0f rpm", g_wait_s, lights, g_player_boost.load() * 100.f, peak);
        g_phase = P_LIGHTS;
    }
    if (g_phase.load() == P_LIGHTS && lights >= 3) {
        begin_launch(t);
        return;
    }
    if (g_wait_s > LIGHTS_TIMEOUT_S) {
        glog("no lights out after %.0f s (lights state %u)", LIGHTS_TIMEOUT_S, lights);
        end_run("timeout");
    }
}

void frame_impl(double dt)
{
    if (const char *why = g_end_request.exchange(nullptr))
        if (g_phase.load() != P_NONE)
            end_run(why);
    if (uintptr_t st = g_pending_st.exchange(0))
        start_run(st);
    int ph = g_phase.load();
    if (ph == P_NONE)
        return;
    double sim = 0;
    for (const auto &l : g_live) {
        double s = l.sim_s.load(RELAXED);
        if (s > sim)
            sim = s;
    }
    float sim_dt = float(sim - g_sim_prev);
    g_sim_prev = sim;
    sim_dt = sim_dt < 0.f ? 0.f : sim_dt > 0.25f ? 0.25f : sim_dt;
    gs::Tuning t = tuning();
    if (ph != P_LAUNCH) {
        g_worst_wait = std::max(g_worst_wait, dt);
        wait_frame(sim_dt, dt, t);
        return;
    }
    g_worst_launch = std::max(g_worst_launch, dt);
    // The cars are moving here, so physics time standing still means a frozen game (pause, flashback view).
    g_frozen = GetTickCount64() - g_tick_ms.load(RELAXED) > FROZEN_MS;
    if (sim_dt > 0.f)
        launch_frame(sim_dt, t);
}

void paint_cb(SwoftyPaint *p, void *);

void try_overlay()
{
    if (g_ov_token || !g_ov || !g_ov->available || !g_ov->on_paint || now_s() < g_ov_retry)
        return;
    g_ov_retry = now_s() + 3.0;
    if (!g_ov->available())
        return;
    uint32_t tok = 0;
    if (g_ov->on_paint(paint_cb, nullptr, &tok) == SWOFTY_ASSET_OK) {
        g_ov_token = tok ? tok : 1;
        glog("overlay: painting");
    }
}

void frame_cb(double dt, void *)
{
    __try {
        frame_impl(dt);
        try_overlay();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        glog("a frame faulted; the run is cancelled");
        end_run("a fault");
    }
}

// ------------------------------------------------------------------------------------------------
// Overlay

void text_c(SwoftyPaint *p, float cx, float y, float size, uint32_t rgba, const char *s)
{
    float w = 0, h = 0;
    g_ov->measure(p, size, s, &w, &h);
    g_ov->text(p, cx - w / 2.f, y, size, rgba, s);
}

void box(SwoftyPaint *p, float x, float y, float w, float h, float r, uint32_t rgba)
{
    if (g_ov_ver >= 2 && g_ov->rect_rounded)
        g_ov->rect_rounded(p, x, y, w, h, r, rgba);
    else
        g_ov->rect(p, x, y, w, h, rgba);
}

void paint_impl(SwoftyPaint *p)
{
    int ph = g_phase.load(RELAXED);
    if (ph == P_NONE || g_frozen.load(RELAXED))
        return;
    float W = 0, H = 0;
    g_ov->size(p, &W, &H);
    float k = H / 1080.f;
    bool on = std::fmod(now_s(), 0.5) < 0.25; // 2 Hz
    float cx = W / 2.f;
    constexpr uint32_t BLUE = 0x1E6BFFFF, DARK = 0x0A0F1CE6, WHITE = 0xFFFFFFFF, AMBER = 0xFFB000FF;
    // The game's own delay, if it has one, runs first: the FIA phase is its last 5 s.
    if (ph == P_PRE && g_delay_left.load(RELAXED) <= float(gs::PRE_START_S) + 0.05f) {
        // The Grid Light Panels and the gantry message (B5.7.2 b).
        if (on) {
            box(p, 40 * k, H * 0.30f, 46 * k, H * 0.40f, 8 * k, BLUE);
            box(p, W - 86 * k, H * 0.30f, 46 * k, H * 0.40f, 8 * k, BLUE);
        }
        box(p, cx - 170 * k, 110 * k, 340 * k, 74 * k, 10 * k, DARK);
        text_c(p, cx, 124 * k, 42 * k, on ? BLUE : WHITE, "PRE-START");
    }
    if ((ph == P_PRE || ph == P_LIGHTS) && s_launch.load(RELAXED) && g_player >= 0) {
        // Low on the screen: the game's start lights stay clear.
        float b = gs::clamp01(g_player_boost.load(RELAXED));
        float bx = cx - 150 * k, by = H * 0.72f, bw = 300 * k, bh = 12 * k;
        box(p, bx - 6 * k, by - 30 * k, bw + 12 * k, 54 * k, 8 * k, DARK);
        text_c(p, cx, by - 27 * k, 20 * k, WHITE, "TURBO SPOOL");
        g_ov->rect(p, bx, by, bw, bh, 0x2A3040FF);
        g_ov->rect(p, bx, by, bw * b, bh, b >= 1.f ? 0x2BD46BFF : BLUE);
    }
    if (ph == P_LAUNCH && on) {
        if (g_player_flag.load(RELAXED)) {
            box(p, cx - 250 * k, 110 * k, 500 * k, 64 * k, 10 * k, DARK);
            text_c(p, cx, 124 * k, 32 * k, AMBER, "LOW-OUTPUT START");
        } else if (g_ahead_flag.load(RELAXED)) {
            box(p, cx - 250 * k, 110 * k, 500 * k, 64 * k, 10 * k, DARK);
            text_c(p, cx, 124 * k, 32 * k, AMBER, "SLOW CAR AHEAD");
            box(p, 40 * k, H * 0.40f, 30 * k, H * 0.20f, 6 * k, AMBER);
            box(p, W - 70 * k, H * 0.40f, 30 * k, H * 0.20f, 6 * k, AMBER);
        }
    }
}

void paint_cb(SwoftyPaint *p, void *)
{
    __try {
        paint_impl(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ------------------------------------------------------------------------------------------------
// Events and init

void on_event(const SwoftyEvent *ev, void *)
{
    // A restart from the results screen has neither event, and keeps the session's arming with it.
    if (!strcmp(ev->name, SWOFTY_EVENT_SESSION_UNLOAD))
        g_armed = false;
    g_end_request = "session over";
}

int on_session_hook(void *, SwoftyHookCall *call)
{
    if (!call || !call->context || !g_hooks || !g_hooks->context)
        return 0;
    char mode[128] = {}, sess[512] = {};
    size_t l = 0;
    int mr = g_hooks->context(call->context, "mode", mode, sizeof mode - 1, &l);
    int sr = g_hooks->context(call->context, "session", sess, sizeof sess - 1, &l);
    // The core reports every online mode and every mode it does not know as "unknown", so this list is the online
    // check too: the lights must stay in sync with the host there. The race types are the two B5.7.2 applies to.
    bool offline = mr == SWOFTY_ASSET_OK && (strstr(mode, "\"grand_prix\"") || strstr(mode, "\"driver_career\"") || strstr(mode, "\"my_team\""));
    bool race = sr == SWOFTY_ASSET_OK && (strstr(sess, "\"type\":\"race\"") || strstr(sess, "\"type\":\"sprint\""));
    g_armed = offline && race;
    glog("session.start: mode %s, session %s -> %s", mode, sess, g_armed.load() ? "armed, the lights check the cars" : "mod idle in this session");
    return 0;
}

bool code_matches(uintptr_t rva, const unsigned char *bytes, size_t n)
{
    auto at = reinterpret_cast<const unsigned char *>(g_api->module_base() + rva);
    __try {
        return memcmp(at, bytes, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> const T *core_table(const char *exp)
{
    HMODULE core = GetModuleHandleW(L"swofty_core.dll");
    auto fn = core ? reinterpret_cast<const T *(*)()>(GetProcAddress(core, exp)) : nullptr;
    return fn ? fn() : nullptr;
}

int init_impl(const SwoftyApi *api)
{
    g_api = api;
    if (api->version < 9 || !api->register_mixins || !api->events) {
        glog("needs Swofty API v9, the core offers v%u; idle", api->version);
        return 1;
    }
    for (const auto &c : CODE_CHECKS)
        if (!code_matches(c.rva, c.bytes, c.n)) {
            glog("%s idle: this game build differs at %s (0x%llx); standard starts", GS_VERSION, c.what, static_cast<unsigned long long>(c.rva));
            return 1;
        }
    read_settings();
    if (api->watch_setting) {
        for (const char *id : {"pre_start", "launch_model", "safety_net", "apply_to_ai", "spool_seconds", "spool_rpm"})
            api->watch_setting(id, [](const char *, const SwoftySettingValue *, void *) { read_settings(); }, nullptr);
    }
    for (auto &w : g_want)
        w = 1.f;
    g_cars = core_table<SwoftyCarOps>("SwoftyCoreCarOps");
    if (g_cars && (g_cars->version < 2 || !g_cars->available || !g_cars->available()))
        g_cars = nullptr;
    if (g_cars && g_cars->field("engine.power_scale", &g_power_field) != SWOFTY_ASSET_OK)
        g_power_field = 0;
    if (g_cars && g_power_field)
        start_layer_worker();
    g_ov = api->overlay;
    if (HMODULE core = GetModuleHandleW(L"swofty_core.dll"))
        if (auto vf = reinterpret_cast<uint32_t (*)()>(GetProcAddress(core, "SwoftyCoreOverlayVersion")))
            g_ov_ver = vf();
    g_hooks = api->hooks;

    const SwoftyMixin mix[] = {
        {"Sess_StartLights_Enter", SWOFTY_MIXIN_WRAP, 0, reinterpret_cast<void *>(&enter_hook), reinterpret_cast<void **>(&g_enter_orig)},
        {"Car_Powertrain_Step", SWOFTY_MIXIN_WRAP, 0, reinterpret_cast<void *>(&powertrain_hook), reinterpret_cast<void **>(&g_powertrain_orig)},
    };
    int mr = api->register_mixins(mix, sizeof mix / sizeof mix[0]);

    uint32_t tok = 0;
    const SwoftyEvents *ev = api->events;
    int er = -1;
    if (ev->available && ev->available()) {
        for (const char *e : {SWOFTY_EVENT_SESSION_END, SWOFTY_EVENT_SESSION_UNLOAD})
            ev->subscribe(e, on_event, nullptr, &tok);
        er = ev->on_frame(frame_cb, nullptr, &tok);
    }
    int hr = g_hooks && g_hooks->attach ? g_hooks->attach("session.start", SWOFTY_PHASE_AFTER, 0, on_session_hook, nullptr, &tok) : -1;
    glog("%s ready: build check ok, mixins %d, on_frame %d, session hook %d, car ops %s, power field %u, layer thread %s, overlay v%u",
         GS_VERSION, mr, er, hr, g_cars ? "yes" : "no", g_power_field, g_layer_event ? "yes" : "no", g_ov_ver);
    return 1;
}

} // namespace

extern "C" __declspec(dllexport) const uint32_t SwoftyAddonApi = SWOFTY_API_VERSION;

extern "C" __declspec(dllexport) int SwoftyAddonInit(const SwoftyApi *api)
{
    if (!api)
        return 0;
    __try {
        return init_impl(api);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
