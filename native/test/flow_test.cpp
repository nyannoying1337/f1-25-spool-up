// flow_test.cpp: runs the addon's real hooks and frame code against a fake game and a fake Swofty core. The fake lights
// update follows the game's code (F1_25.exe 0x142d8fcc0): countdown, sequence start, and "finished" read every update.
// Also checks the build-check bytes against the real exe when it is installed.
#include "../src/grid_start.cpp"

#include <algorithm>
#include <cstdarg>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

static int g_fails, g_checks;
static std::atomic<long> g_faults{0};
static LONG CALLBACK count_faults(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        g_faults++;
    return EXCEPTION_CONTINUE_SEARCH;
}
static const char *g_test = "";
static void check(bool ok, const char *what)
{
    g_checks++;
    if (!ok) {
        printf("FAIL [%s] %s\n", g_test, what);
        g_fails++;
    }
}

struct Mem {
    std::vector<uint8_t> b;
    explicit Mem(size_t n) : b(n, 0) {}
    uintptr_t p() { return reinterpret_cast<uintptr_t>(b.data()); }
    template <class T> T &at(uintptr_t off) { return *reinterpret_cast<T *>(b.data() + off); }
};

// ------------------------------------------------------------------------------------------------
// The fake core

static std::vector<std::string> g_logs;
static std::mutex g_log_lock;
static bool g_verbose;

static void fake_log(const char *fmt, ...)
{
    char b[1200];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    std::lock_guard<std::mutex> g(g_log_lock);
    g_logs.emplace_back(b);
    if (g_verbose)
        printf("    %s\n", b);
}

static std::string log_line(const char *part)
{
    std::lock_guard<std::mutex> g(g_log_lock);
    for (const std::string &s : g_logs)
        if (s.find(part) != std::string::npos)
            return s;
    return {};
}

static bool logged(const char *part)
{
    std::lock_guard<std::mutex> g(g_log_lock);
    return std::any_of(g_logs.begin(), g_logs.end(), [&](const std::string &s) { return s.find(part) != std::string::npos; });
}

static int log_count(const char *part)
{
    std::lock_guard<std::mutex> g(g_log_lock);
    return int(std::count_if(g_logs.begin(), g_logs.end(), [&](const std::string &s) { return s.find(part) != std::string::npos; }));
}

struct FakeLayer {
    uint32_t car;
    float value;
};
static std::map<uint32_t, FakeLayer> g_fake_layers;
static std::mutex g_layer_lock;
static uint32_t g_next_token = 1;
static std::string g_layer_owner;
static int32_t g_layer_priority;
static DWORD g_layer_tid;

static int cars_available() { return 1; }
static int cars_field(const char *path, uint32_t *id)
{
    if (strcmp(path, "engine.power_scale"))
        return SWOFTY_ASSET_INVALID;
    *id = 32;
    return SWOFTY_ASSET_OK;
}
static int cars_layer(const char *owner, uint32_t index, uint32_t field, uint32_t op, float value, int32_t priority, uint32_t *token)
{
    if (field != 32 || op != SWOFTY_CAR_OP_SCALE)
        return SWOFTY_ASSET_INVALID;
    std::lock_guard<std::mutex> g(g_layer_lock);
    g_layer_owner = owner;
    g_layer_priority = priority;
    g_layer_tid = GetCurrentThreadId();
    *token = g_next_token++;
    g_fake_layers[*token] = {index, value};
    return SWOFTY_ASSET_OK;
}
static int cars_unlayer(uint32_t token)
{
    std::lock_guard<std::mutex> g(g_layer_lock);
    return g_fake_layers.erase(token) ? SWOFTY_ASSET_OK : SWOFTY_ASSET_INVALID;
}
static float layered_power(uint32_t car)
{
    std::lock_guard<std::mutex> g(g_layer_lock);
    float v = 1.f;
    for (auto &[tok, l] : g_fake_layers)
        if (l.car == car)
            v *= l.value;
    return v;
}
static size_t layer_count()
{
    std::lock_guard<std::mutex> g(g_layer_lock);
    return g_fake_layers.size();
}
static bool has_layer(uint32_t car)
{
    std::lock_guard<std::mutex> g(g_layer_lock);
    for (auto &[tok, l] : g_fake_layers)
        if (l.car == car)
            return true;
    return false;
}
static int cars_get(uint32_t index, const void *, uint32_t, float *value)
{
    *value = layered_power(index);
    return SWOFTY_ASSET_OK;
}
static int cars_count(uint32_t *n);
static int cars_car(uint32_t index, void **handle, uint32_t *is_ai);
static SwoftyCarOps g_fake_cars = {2, cars_available, cars_count, cars_car, cars_get, nullptr, cars_field, cars_layer, cars_unlayer, nullptr};

// The fake overlay records every shape. A circle keeps its centre in x, y and its radius in w.
struct Drawn {
    char kind; // r rect, R rounded rect, o outline, g gradient, c circle, p polygon, l line, t text
    float x, y, w, h;
    uint32_t rgba;
    std::string text;
    uint32_t align;
};
static std::vector<Drawn> g_drawn;
static std::vector<std::string> g_painted;
static float g_screen_w = 1920, g_screen_h = 1080;
static int g_clips;
static void ov_size(const SwoftyPaint *, float *w, float *h)
{
    *w = g_screen_w;
    *h = g_screen_h;
}
static void ov_rect(SwoftyPaint *, float x, float y, float w, float h, uint32_t rgba) { g_drawn.push_back({'r', x, y, w, h, rgba, {}, 0}); }
static void ov_rect_outline(SwoftyPaint *, float x, float y, float w, float h, float, uint32_t rgba) { g_drawn.push_back({'o', x, y, w, h, rgba, {}, 0}); }
static void ov_line(SwoftyPaint *, float x0, float y0, float x1, float y1, float, uint32_t rgba) { g_drawn.push_back({'l', x0, y0, x1, y1, rgba, {}, 0}); }
static void ov_text(SwoftyPaint *, float x, float y, float px, uint32_t rgba, const char *s)
{
    g_painted.emplace_back(s);
    g_drawn.push_back({'t', x, y, px, 0, rgba, s, 0});
}
static void ov_measure(const SwoftyPaint *, float, const char *, float *w, float *h)
{
    *w = 100;
    *h = 20;
}
static void ov_rect_rounded(SwoftyPaint *, float x, float y, float w, float h, float, uint32_t rgba) { g_drawn.push_back({'R', x, y, w, h, rgba, {}, 0}); }
static void ov_rect_rounded_outline(SwoftyPaint *, float x, float y, float w, float h, float, float, uint32_t rgba) { g_drawn.push_back({'o', x, y, w, h, rgba, {}, 0}); }
static void ov_rect_gradient(SwoftyPaint *, float x, float y, float w, float h, float, uint32_t from, uint32_t to, uint32_t flags)
{
    check(flags == SWOFTY_GRADIENT_VERTICAL && (from & 0xff) == 0 && (from >> 8) == (to >> 8), "a glow: clear at the top, the colour at the bottom");
    g_drawn.push_back({'g', x, y, w, h, to, {}, 0});
}
static void ov_circle(SwoftyPaint *, float cx, float cy, float r, uint32_t rgba) { g_drawn.push_back({'c', cx, cy, r, r, rgba, {}, 0}); }
static void ov_polygon(SwoftyPaint *, const float *xy, uint32_t n, uint32_t rgba) { g_drawn.push_back({'p', xy[0], xy[1], float(n), 0, rgba, {}, 0}); }
static void ov_text_styled(SwoftyPaint *, float x, float y, const SwoftyTextStyle *st, const char *s)
{
    check(st->size == sizeof *st && st->px > 0.f, "a whole text style with a size");
    g_painted.emplace_back(s);
    g_drawn.push_back({'t', x, y, st->px, 0, st->rgba, s, st->align});
}
static void ov_push_clip(SwoftyPaint *, float, float, float, float) { g_clips++; }
static void ov_pop_clip(SwoftyPaint *) { g_clips--; }
static int ov_available() { return 1; }
static SwoftyPaintFn g_paint_fn;
static int ov_on_paint(SwoftyPaintFn fn, void *, uint32_t *tok)
{
    g_paint_fn = fn;
    *tok = 7;
    return SWOFTY_ASSET_OK;
}
static SwoftyOverlay make_overlay(bool full)
{
    SwoftyOverlay o{};
    o.available = ov_available;
    o.on_paint = ov_on_paint;
    o.size = ov_size;
    o.rect = ov_rect;
    o.rect_outline = ov_rect_outline;
    o.line = ov_line;
    o.text = ov_text;
    o.measure = ov_measure;
    if (full) {
        o.rect_rounded = ov_rect_rounded;
        o.rect_rounded_outline = ov_rect_rounded_outline;
        o.rect_gradient = ov_rect_gradient;
        o.circle = ov_circle;
        o.polygon = ov_polygon;
        o.text_styled = ov_text_styled;
        o.push_clip = ov_push_clip;
        o.pop_clip = ov_pop_clip;
    } else {
        // An older core's table ends before the v2 entries: what follows it is not a function.
        auto *v2_part = reinterpret_cast<uint8_t *>(&o.rect_rounded);
        memset(v2_part, 0x5A, size_t(reinterpret_cast<uint8_t *>(&o + 1) - v2_part));
    }
    return o;
}
static SwoftyOverlay g_fake_overlay = make_overlay(true), g_fake_overlay_v1 = make_overlay(false);

static void paint()
{
    g_painted.clear();
    g_drawn.clear();
    paint_cb(nullptr, nullptr);
    check(g_clips == 0, "every clip is taken off again");
}
static bool painted(const char *what)
{
    paint();
    return std::find(g_painted.begin(), g_painted.end(), what) != g_painted.end();
}
static const Drawn *drawn_text(const char *what)
{
    for (const Drawn &d : g_drawn)
        if (d.kind == 't' && d.text == what)
            return &d;
    return nullptr;
}
static int drawn_count(char kind, uint32_t rgba)
{
    return int(std::count_if(g_drawn.begin(), g_drawn.end(), [&](const Drawn &d) { return d.kind == kind && d.rgba == rgba; }));
}

// The start lights, found by their dark rim: a circle, or a square on an overlay without circles. The stand-in
// message box has a small one, under 24 px at 1080p.
struct Light {
    float x, y, r;
    bool lit;
};
static std::vector<Light> lights_drawn()
{
    paint();
    float k = std::min(g_screen_w / 1920.f, g_screen_h / 1080.f);
    std::vector<Light> v;
    for (size_t i = 0; i < g_drawn.size(); i++) {
        const Drawn &d = g_drawn[i];
        float r = d.kind == 'c' ? d.w : d.kind == 'r' ? d.w / 2.f : 0.f;
        if (d.rgba != 0x1D2027FF || r < 24.f * k)
            continue;
        bool lit = i + 3 < g_drawn.size() && g_drawn[i + 3].rgba == 0x163A9EFF; // the face after the rim's three discs
        v.push_back({d.kind == 'c' ? d.x : d.x + r, d.kind == 'c' ? d.y : d.y + r, r, lit});
    }
    return v;
}
static bool lights_on_screen() { return lights_drawn().size() == 5; }

// The fake game HUD: leaves as text ("1" and "0" for a bool), and the messages.
struct Toast {
    std::string text, detail, icon;
    float seconds;
};
static std::vector<Toast> g_toasts;
static std::map<std::string, std::string> g_leaves;
static int g_leaf_writes, g_hud_up = 1, g_hud_set_result = SWOFTY_ASSET_OK, g_hud_toast_result = SWOFTY_ASSET_OK;
static int hud_available() { return g_hud_up; }
static int hud_toast(const char *text, const char *detail, float seconds, const char *icon)
{
    if (g_hud_toast_result == SWOFTY_ASSET_OK)
        g_toasts.push_back({text, detail ? detail : "", icon ? icon : "(null)", seconds});
    return g_hud_toast_result;
}
static int hud_set(const char *path, uint32_t type, const void *value)
{
    check(!strncmp(path, "ui_menu_data.hud_", 17), "only the HUD's own leaves");
    if (g_hud_set_result != SWOFTY_ASSET_OK)
        return g_hud_set_result;
    g_leaf_writes++;
    g_leaves[path] = type == SWOFTY_HUD_TEXT ? std::string(static_cast<const char *>(value)) : std::to_string(*static_cast<const int32_t *>(value));
    return SWOFTY_ASSET_OK;
}
static SwoftyHud g_fake_hud = {1, hud_available, hud_toast, hud_set};
static std::string leaf(const char *path)
{
    auto it = g_leaves.find(path);
    return it == g_leaves.end() ? std::string() : it->second;
}
static bool lap_says_pre_start()
{
    return leaf(LAP_TEXT) == "PRE-START" && leaf(LAP_LOC_TAG) == "0" && leaf(LAP_NUMBER) == "0" && leaf(LAP_TOTAL) == "0";
}
static bool lap_number_back() { return leaf(LAP_NUMBER) == "1" && leaf(LAP_TOTAL) == "1" && leaf(LAP_LOC_TAG) == "1"; }
static int toasts_of(const char *text)
{
    return int(std::count_if(g_toasts.begin(), g_toasts.end(), [&](const Toast &t) { return t.text == text; }));
}

static std::string g_ctx_mode, g_ctx_session;
static int hooks_context(const void *, const char *part, char *out, size_t cap, size_t *len)
{
    const std::string &v = !strcmp(part, "mode") ? g_ctx_mode : g_ctx_session;
    if (v.size() >= cap)
        return SWOFTY_TOO_SMALL;
    memcpy(out, v.c_str(), v.size() + 1);
    *len = v.size();
    return SWOFTY_ASSET_OK;
}
static SwoftyHookFn g_session_fn;
static int hooks_attach(const char *point, uint32_t phase, int32_t, SwoftyHookFn fn, void *, uint32_t *token)
{
    if (!strcmp(point, "session.start") && phase == SWOFTY_PHASE_AFTER)
        g_session_fn = fn;
    *token = 1;
    return SWOFTY_ASSET_OK;
}
static SwoftyHooks make_hooks()
{
    SwoftyHooks h{};
    h.version = 1;
    h.attach = hooks_attach;
    h.context = hooks_context;
    return h;
}
static SwoftyHooks g_fake_hooks = make_hooks();

static std::vector<std::string> g_events;
static SwoftyEventFn g_event_fn;
static SwoftyFrameFn g_frame_fn;
static int ev_available() { return 1; }
static int ev_subscribe(const char *e, SwoftyEventFn fn, void *, uint32_t *tok)
{
    g_events.emplace_back(e);
    g_event_fn = fn;
    *tok = 2;
    return SWOFTY_ASSET_OK;
}
static int ev_on_frame(SwoftyFrameFn fn, void *, uint32_t *tok)
{
    g_frame_fn = fn;
    *tok = 3;
    return SWOFTY_ASSET_OK;
}
static SwoftyEvents g_fake_events = {ev_available, ev_subscribe, ev_on_frame, nullptr};

static std::map<std::string, double> g_settings;
static int api_setting(const char *id, uint32_t type, SwoftySettingValue *out)
{
    auto it = g_settings.find(id);
    if (it == g_settings.end())
        return 0;
    out->type = type;
    out->toggle = it->second != 0;
    out->slider = it->second;
    return 1;
}
static int api_watch(const char *, SwoftySettingFn, void *) { return 1; }

static std::vector<SwoftyMixin> g_mixins;
static void *g_fake_enter_fn, *g_fake_step_fn, *g_fake_zone_fn;
static int api_mixins(const SwoftyMixin *m, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        g_mixins.push_back(m[i]);
        if (m[i].orig)
            *m[i].orig = !strcmp(m[i].target, "Sess_StartLights_Enter") ? g_fake_enter_fn
                         : !strcmp(m[i].target, "Sess_MarshalLights_UpdateZoneShader") ? g_fake_zone_fn
                                                                                       : g_fake_step_fn;
    }
    return 1;
}
static uintptr_t g_image;
static uintptr_t api_base() { return g_image; }

// ------------------------------------------------------------------------------------------------
// The fake game

constexpr uint32_t PLAYER = 5, RACERS = 22, SAFETY_CAR = 22;
constexpr float ERS_2025 = 126.3e3f, ERS_2026 = 350e3f;
constexpr uint32_t HANDLING_2025 = 8, HANDLING_2026 = 9, HANDLING_SAFETY_CAR = 1;

struct Car {
    Mem car{0xa000}, dyn{0xb00}, engine{0x2600}, ers{0x1a80}, physics{0x400}, rigid{0x40}, body{0x300};
    float kmh = 0, rpm = 4000, deploy_wanted = 350e3f, real_deploy = 0, cap_seen = -1.f, x = 0, z = 0;
    uint32_t steps = 0;
    bool no_ers = false;
    std::vector<uint8_t> curves; // the deploy curves as the game loaded them
};
static Car *g_car; // [RACERS + 1]

// The core's car table, as op_count and op_car answer. The real handle is the game's car.
static bool g_table_down;
static int g_table_late = -1;
static uintptr_t g_table_freed; // car 1's handle, when set: a car that is already gone
static int cars_count(uint32_t *n)
{
    if (g_table_down)
        return SWOFTY_ASSET_UNAVAILABLE;
    *n = RACERS + 1;
    return SWOFTY_ASSET_OK;
}
static int cars_car(uint32_t index, void **handle, uint32_t *is_ai)
{
    if (index > RACERS || int(index) == g_table_late)
        return SWOFTY_ASSET_LATE;
    *handle = g_table_freed && index == 1 ? reinterpret_cast<void *>(g_table_freed) : reinterpret_cast<void *>(g_car[index].car.p());
    if (is_ai)
        *is_ai = g_car[index].car.at<uint32_t>(CAR_IS_AI);
    return SWOFTY_ASSET_OK;
}

static Mem *g_st, *g_sl, *g_session, *g_desc;
static float g_lights_t, g_game_delay;
static float g_hold_s = 1.f;
static bool g_routed;     // the update said "StartLights Complete" and left the state
static int g_routes;

static uintptr_t fake_enter(void *self, void *, void *, void *)
{
    Mem &st = *g_st;
    if (reinterpret_cast<uintptr_t>(self) == st.p() && st.at<uint8_t>(ST_ONCE) == 0) {
        st.at<uint8_t>(ST_ONCE) = 1;
        st.at<float>(ST_DELAY) = g_game_delay;
    }
    return 1;
}

static Car &car_of(void *pt)
{
    uintptr_t car = *reinterpret_cast<uintptr_t *>(reinterpret_cast<uintptr_t>(pt) + DYN_CAR);
    return g_car[*reinterpret_cast<uint8_t *>(car + CAR_VEHICLE_ID)];
}

// SpeedDeploySpline of every F1 2026 handling file, as far as a launch goes: m/s to a share of the max deploy.
static float deploy_curve(float ms)
{
    if (ms <= 13.89f)
        return 0.f;
    return ms >= 35.f ? 0.39f : 0.39f * (ms - 13.89f) / (35.f - 13.89f);
}

static float obf_at(Mem &m, uintptr_t at)
{
    uint32_t v = m.at<uint32_t>(at) ^ m.at<uint32_t>(at + OBF_KEY);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

// Car_Spline_EvalObfuscated, 0x142e88290, on the car's own memory, branch by branch. Below a last index of 0 the game reads
// a point list outside the record: here, the deploy curve above.
static float fake_eval(Mem &ers, uintptr_t curve, float x)
{
    int32_t last = ers.at<int32_t>(curve + SPLINE_LAST);
    if (last < 0)
        return deploy_curve(x);
    float idx = x * obf_at(ers, curve + SPLINE_SCALE) + obf_at(ers, curve + SPLINE_OFFSET);
    if (0.f >= idx)
        return obf_at(ers, curve + SPLINE_SAMPLES);
    int i = int(idx);
    if (last <= i)
        return obf_at(ers, curve + SPLINE_SAMPLES + 16 * uintptr_t(last));
    float y0 = obf_at(ers, curve + SPLINE_SAMPLES + 16 * uintptr_t(i)), y1 = obf_at(ers, curve + SPLINE_SAMPLES + 16 * uintptr_t(i + 1));
    return y0 + (y1 - y0) * (idx - float(i));
}

// The end of the game's deploy code: the result never exceeds curve x max deploy (0x1446e2a3d, 0x1446e2c30).
static void fake_step(void *pt)
{
    Car &c = car_of(pt);
    c.steps++;
    if (c.no_ers)
        return;
    c.cap_seen = c.ers.at<float>(ERS_MAX_DEPLOY);
    if (c.cap_seen != c.real_deploy)
        check(false, "the game's call sees the car's own max deploy");
    uintptr_t curve = c.ers.at<uint8_t>(ERS_CURVE_FLAG) ? ERS_CURVE_2 : ERS_CURVE;
    c.ers.at<float>(ERS_DEPLOY) = std::min(c.deploy_wanted, fake_eval(c.ers, curve, c.kmh / 3.6f) * c.cap_seen);
}

constexpr size_t CURVES_SIZE = 2 * (ERS_CURVE_2 - ERS_CURVE);
static void keep_curves(Car &c) { c.curves.assign(c.ers.b.begin() + ERS_CURVE, c.ers.b.begin() + ERS_CURVE + CURVES_SIZE); }
static bool curves_intact(Car &c) { return c.curves.size() == CURVES_SIZE && !memcmp(c.curves.data(), c.ers.b.data() + ERS_CURVE, CURVES_SIZE); }

static void set_obf(Mem &m, uintptr_t at, float v, uint32_t key)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof bits);
    m.at<uint32_t>(at + OBF_KEY) = key;
    m.at<uint32_t>(at) = bits ^ key;
}

// Point lists as the loader leaves them (0x14466db1b): scale and offset of 0, keys that are not 0.
static void load_point_curves(Car &c, uint32_t id)
{
    for (uintptr_t curve : {ERS_CURVE, ERS_CURVE_2}) {
        c.ers.at<int32_t>(curve + SPLINE_LAST) = -1;
        set_obf(c.ers, curve + SPLINE_SCALE, 0.f, 0x5a5a0000u + id);
        set_obf(c.ers, curve + SPLINE_OFFSET, 0.f, 0x3c3c0000u + id);
        set_obf(c.ers, curve + SPLINE_SAMPLES, 0.25f, 0x7e7e0000u + id);
    }
    c.ers.at<uint8_t>(ERS_CURVE_FLAG) = 0;
    keep_curves(c);
}

// The eval's other form: evenly spaced samples inside the record, 10 m/s apart.
static void load_uniform_curve(Car &c)
{
    const float y[] = {0.f, 0.f, 0.2f, 0.39f, 0.39f};
    c.ers.at<int32_t>(ERS_CURVE + SPLINE_LAST) = 4;
    set_obf(c.ers, ERS_CURVE + SPLINE_SCALE, 0.1f, 0x11110000u);
    set_obf(c.ers, ERS_CURVE + SPLINE_OFFSET, 0.f, 0x22220000u);
    for (uintptr_t i = 0; i < 5; i++)
        set_obf(c.ers, ERS_CURVE + SPLINE_SAMPLES + 16 * i, y[i], 0x33330000u + uint32_t(i));
    keep_curves(c);
}

// The game's marshal lights system, as far as this mod goes (0x142c77470, 0x142c786b0). Each tick it writes every zone's
// colour and mode, then calls the zone update, which sends them to the panel's shader. Its update flags (0xe, set at
// 0x1407f1555) leave out the pause state's 0x10, so it stands still in a pause.
constexpr uint32_t PANEL_ZONES = 4;
constexpr float GAME_COLOUR[4] = {0.f, 1.f, 0.f, 0.f};
constexpr float GAME_MODE[4] = {0.992f, 0.551f, 0.074f, 2.f}; // differs from the mod's in both halves
struct PanelsSeen {
    uint32_t calls, blue, off, game, other, wrong_args;
};
static PanelsSeen g_seen;
static Mem *g_marshal, *g_zones;

static void fake_zone_update(void *sys, void *zone)
{
    g_seen.calls++;
    auto z = reinterpret_cast<uintptr_t>(zone);
    bool known = false;
    for (uint32_t i = 0; i < PANEL_ZONES; i++)
        known = known || g_zones[i].p() == z;
    if (reinterpret_cast<uintptr_t>(sys) != g_marshal->p() || !known)
        g_seen.wrong_args++;
    auto is = [&](uintptr_t at, const float *v) { return !memcmp(reinterpret_cast<const void *>(z + at), v, 16); };
    if (is(ZONE_COLOUR, PANEL_BLUE) && is(ZONE_MODE, PANEL_LIT))
        g_seen.blue++;
    else if (is(ZONE_COLOUR, PANEL_OFF) && is(ZONE_MODE, PANEL_OFF))
        g_seen.off++;
    else if (is(ZONE_COLOUR, GAME_COLOUR) && is(ZONE_MODE, GAME_MODE))
        g_seen.game++;
    else
        g_seen.other++;
}

static void fake_marshal()
{
    if (g_session->at<uint32_t>(SESSION_STATE) == SESSION_PAUSED)
        return;
    for (uint32_t i = 0; i < PANEL_ZONES; i++) {
        Mem &z = g_zones[i];
        memcpy(&z.at<float>(ZONE_COLOUR), GAME_COLOUR, sizeof GAME_COLOUR);
        memcpy(&z.at<float>(ZONE_MODE), GAME_MODE, sizeof GAME_MODE);
        zone_hook(reinterpret_cast<void *>(g_marshal->p()), reinterpret_cast<void *>(z.p()));
    }
}

static void build_world()
{
    static Car cars[RACERS + 1];
    static Mem st(0x1000), sl(0xc40), session(0x200), desc(0x10);
    static Mem marshal(0x100), model(0x100), model_not_in_world(0x100), instance(0x10);
    static Mem zones[PANEL_ZONES] = {Mem(0x50), Mem(0x50), Mem(0x50), Mem(0x50)};
    g_marshal = &marshal;
    g_zones = zones;
    model.at<uintptr_t>(MODEL_INSTANCE) = instance.p();
    zones[0].at<uintptr_t>(ZONE_MODEL) = model.p();
    zones[1].at<uintptr_t>(ZONE_MODEL) = model.p();
    zones[2].at<uintptr_t>(ZONE_MODEL) = model_not_in_world.p();
    // The last zone has no panel model.
    g_car = cars;
    g_st = &st;
    g_sl = &sl;
    g_session = &session;
    g_desc = &desc;
    st.at<uintptr_t>(0) = 0x1457e25b0;
    st.at<uintptr_t>(ST_LIGHTS) = sl.p();
    st.at<uintptr_t>(ST_SESSION) = session.p();
    session.at<uintptr_t>(SESSION_DESC) = desc.p();
    for (uint32_t i = 0; i <= RACERS; i++) {
        Car &c = cars[i];
        c.dyn.at<uintptr_t>(DYN_CAR) = c.car.p();
        c.dyn.at<uintptr_t>(DYN_ENGINE) = c.engine.p();
        c.engine.at<uintptr_t>(SYS_OWNER) = c.car.p();
        c.car.at<uint8_t>(CAR_VEHICLE_ID) = uint8_t(i);
        c.car.at<uintptr_t>(CAR_CHASSIS) = c.physics.p();
        c.physics.at<uintptr_t>(CH_RIGID) = c.rigid.p();
        c.rigid.at<uintptr_t>(RIGID_BODY) = c.body.p();
        c.ers.at<uintptr_t>(SYS_OWNER) = c.car.p();
    }
}

static void set_car(uint32_t i, float deploy_w, uint32_t handling)
{
    g_car[i].ers.at<float>(ERS_MAX_DEPLOY) = deploy_w;
    g_car[i].real_deploy = deploy_w;
    g_car[i].car.at<uint32_t>(CAR_HANDLING) = handling;
}

static void set_grid(float deploy_w, uint32_t handling)
{
    for (uint32_t i = 0; i < RACERS; i++)
        set_car(i, deploy_w, handling);
}

// The game's lights update, 0x142d8fcc0, reduced to what this mod depends on.
static void fake_update(double dt)
{
    if (g_routed)
        return;
    Mem &st = *g_st, &sl = *g_sl;
    uint8_t &state = sl.at<uint8_t>(SL_STATE);
    uint8_t &mode = sl.at<uint8_t>(SL_MODE);
    bool paused = g_session->at<uint32_t>(SESSION_STATE) == SESSION_PAUSED;
    if (!paused) {
        float &delay = st.at<float>(ST_DELAY);
        if (delay <= 0.f) {
            if (state == 0 || mode == SL_MODE_FORMATION) {
                mode = g_desc->at<uint8_t>(0) == 7 ? 1 : 0; // Game_StartLights_ResetSequence
                state = 1;
                g_lights_t = 0;
            }
        } else {
            delay -= float(dt);
        }
    }
    if (!sl.at<uint8_t>(SL_ENABLED)) {
        g_routed = true; // "StartLights skipped"
        g_routes++;
        return;
    }
    if (paused)
        return;
    // The tick
    if (state == 1 || state == 2) {
        g_lights_t += float(dt);
        if (state == 1 && g_lights_t >= 5.f)
            state = 2;
        if (state == 2 && g_lights_t >= 5.f + g_hold_s)
            state = 3; // MessageStartLightsComplete: lights out
    } else if (state == 3) {
        state = 4;
    }
    if (state == 4) {
        g_routed = true; // "StartLights Complete"
        g_routes++;
    }
}

static bool g_check_deploy = true, g_check_curves = true;
static void run_steps(double dt, bool time_passes)
{
    int n = time_passes ? std::max(1, int(dt * 360.0)) : 1;
    float sub = time_passes ? float(dt) / float(n) : 0.f;
    for (uint32_t i = 0; i <= RACERS; i++) {
        Car &c = g_car[i];
        c.dyn.at<float>(DYN_DT) = sub;
        c.engine.at<float>(ENGINE_RAD_S) = c.rpm * RAD_S_PER_RPM;
        c.body.at<float>(BODY_VEL + 8) = c.kmh / 3.6f;
        c.body.at<float>(BODY_POS) = c.x;
        c.body.at<float>(BODY_POS + 8) = c.z;
        float before = c.ers.at<float>(ERS_MAX_DEPLOY);
        for (int s = 0; s < n; s++) {
            powertrain_hook(reinterpret_cast<void *>(c.dyn.p()));
            if (g_check_deploy && !c.no_ers && c.ers.at<float>(ERS_MAX_DEPLOY) != before) {
                check(false, "the mod never writes the max deploy");
                g_check_deploy = false;
            }
        }
        if (g_check_curves && !c.no_ers && !curves_intact(c)) {
            check(false, "the deploy curves are the game's own after the step");
            g_check_curves = false;
        }
    }
}

static void tick(double dt = 1.0 / 60.0, bool update = true)
{
    if (update) {
        fake_update(dt);
        fake_marshal();
    }
    run_steps(dt, g_session->at<uint32_t>(SESSION_STATE) != SESSION_PAUSED);
    frame_cb(dt, nullptr);
}

static void ticks(double seconds, double dt = 1.0 / 60.0)
{
    for (int i = 0, n = int(seconds / dt + 0.5); i < n; i++)
        tick(dt);
}

static void wait_layers()
{
    for (int i = 0; i < 400 && g_layer_done.load() < g_layer_posts.load(); i++)
        Sleep(5);
    check(g_layer_done.load() >= g_layer_posts.load(), "the layer thread catches up");
}

static void enter() { enter_hook(reinterpret_cast<void *>(g_st->p()), nullptr, nullptr, nullptr); }

static void session_start(const char *mode, const char *type, const char *session = "race")
{
    g_ctx_mode = std::string("\"") + mode + "\"";
    g_ctx_session = std::string("{\"type\":\"") + type + "\",\"session\":\"" + session + "\",\"group\":0,\"track\":\"game/montreal\"}";
    SwoftyHookCall call{};
    call.context = "ctx";
    on_session_hook(nullptr, &call);
}

static void fire(const char *name)
{
    SwoftyEvent e{};
    e.name = name;
    on_event(&e, nullptr);
}

// A race weekend on a 2026 grid: 22 F1 2026 cars and a safety car with no ERS.
static void reset(const char *name)
{
    g_test = name;
    g_cars = &g_fake_cars;
    g_end_request = "test reset";
    frame_cb(0.0, nullptr);
    wait_layers();
    check(layer_count() == 0, "no layer is left from the test before");
    g_ov = &g_fake_overlay;
    g_ov_ver = 3;
    g_screen_w = 1920;
    g_screen_h = 1080;
    g_hud = &g_fake_hud;
    g_hud_up = 1;
    g_hud_set_result = g_hud_toast_result = SWOFTY_ASSET_OK;
    g_toasts.clear();
    g_leaves.clear();
    g_leaf_writes = 0;
    g_settings.clear();
    s_pre_start = s_launch = s_net = s_ai = true;
    s_spool = 3.0f;
    s_spool_rpm = 10000.f;
    g_offline = false;
    g_race_label = false;
    g_pending_st = 0;
    g_table_down = false;
    g_table_late = -1;
    g_table_freed = 0;
    g_rng.seed(2026);
    Mem &st = *g_st, &sl = *g_sl;
    st.at<uint8_t>(ST_ONCE) = 0;
    st.at<float>(ST_DELAY) = 0;
    st.at<uintptr_t>(0) = 0x1457e25b0;
    sl.at<uint8_t>(SL_STATE) = 0;
    sl.at<uint8_t>(SL_MODE) = 0;
    sl.at<uint8_t>(SL_ENABLED) = 1;
    g_session->at<uint32_t>(SESSION_STATE) = 0;
    g_desc->at<uint8_t>(0) = 0;
    g_desc->at<uint32_t>(4) = 15;
    g_game_delay = 0;
    g_hold_s = 1.f;
    g_routed = false;
    g_routes = 0;
    g_check_deploy = true;
    g_check_curves = true;
    g_marshal->at<uint8_t>(MARSHAL_SHADERS) = 1;
    g_seen = PanelsSeen{};
    for (uint32_t i = 0; i <= RACERS; i++) {
        Car &c = g_car[i];
        load_point_curves(c, i);
        c.kmh = 0;
        c.rpm = 4000;
        c.x = 0;
        c.z = -float(i) * 8.f;
        c.cap_seen = -1.f;
        c.steps = 0;
        c.no_ers = i == SAFETY_CAR;
        c.car.at<uintptr_t>(CAR_ERS) = c.no_ers ? 0 : c.ers.p();
        c.ers.at<uintptr_t>(SYS_OWNER) = c.car.p();
        c.car.at<uint32_t>(CAR_IS_AI) = i == PLAYER ? 0 : 1;
    }
    set_grid(ERS_2026, HANDLING_2026);
    g_car[SAFETY_CAR].car.at<uint32_t>(CAR_HANDLING) = HANDLING_SAFETY_CAR;
    std::lock_guard<std::mutex> g(g_log_lock);
    g_logs.clear();
}

// To lights out, with the player holding the given revs.
static void run_to_lights_out(float rpm)
{
    g_car[PLAYER].rpm = rpm;
    for (int i = 0; i < 60 * 30 && g_phase.load() != P_LAUNCH; i++)
        tick();
    check(g_phase.load() == P_LAUNCH, "reached lights out");
}

static void launch_all(float rise_kmh_per_s, double seconds, double dt = 1.0 / 60.0)
{
    for (int i = 0, n = int(seconds / dt + 0.5); i < n && g_phase.load() != P_NONE; i++) {
        for (uint32_t c = 0; c < RACERS; c++)
            g_car[c].kmh += rise_kmh_per_s * float(dt);
        tick(dt);
    }
}

// ------------------------------------------------------------------------------------------------
// Tests

static void test_normal_start()
{
    reset("normal start");
    session_start("grand_prix", "race");
    check(g_offline.load(), "an offline mode");
    enter();
    check(std::fabs(g_st->at<float>(ST_DELAY) - 5.f) < 1e-4f, "5 s added to the game's delay of 0");
    check(logged("23 cars, 22 F1 2026, 0 others"), "the grid is logged: the safety car has no ERS and does not count");
    tick();
    check(g_phase.load() == P_PRE, "Pre-Start");
    tick();
    check(g_car[PLAYER].cap_seen == ERS_2026 && g_car[3].cap_seen == ERS_2026, "the game's own max deploy, on an AI car too");
    check(lights_on_screen(), "the five blue lights show");
    check(lap_says_pre_start(), "the game's lap counter says PRE-START");
    check(!painted("PRE-START"), "the game draws the words: no stand-in box");
    check(painted("TURBO SPOOL"), "the spool gauge shows");
    g_car[PLAYER].rpm = 11000;
    ticks(4.5);
    check(g_phase.load() == P_PRE, "still Pre-Start after 4.5 s");
    check(g_sl->at<uint8_t>(SL_STATE) == 0, "the lights wait");
    check(g_player == int(PLAYER), "the player car is the one the AI does not drive");
    check(g_player_boost.load() > 0.99f, "4.5 s at revs is full boost");
    check(!g_routed, "the game stays in the lights state");
    ticks(0.6);
    check(g_sl->at<uint8_t>(SL_STATE) == 1, "the lights start 5 s after enter");
    check(g_phase.load() == P_LIGHTS, "the run follows the lights");
    check(!lights_on_screen(), "the blue lights are gone");
    check(lap_number_back(), "the lap counter has its lap number back");
    check(painted("TURBO SPOOL"), "the spool gauge stays");
    check(g_wait_sim_s > 5.0 && g_wait_sim_s < 5.4, "physics time matches the time played");
    run_to_lights_out(11000);
    wait_layers();
    check(!has_layer(PLAYER), "player at full boost: the engine is untouched");
    check(!has_layer(SAFETY_CAR), "no layer on the safety car");
    check(layer_count() >= 1 && layer_count() < RACERS, "a layer on the AI cars whose random boost is low enough to show");
    for (uint32_t c = 0; c < RACERS; c++)
        check(layered_power(c) >= 0.349f, "no car is below the power of no boost at all");
    check(logged("AI spool at lights out: "), "the AI's spread is logged");
    check(g_layer_owner == "nick.grid-start-2026" && g_layer_priority == 40, "owner and priority: under Terminal Damage's 50");
    check(logged("engine.power_scale layer on car"), "the first layer is logged");
    check(g_layer_tid != GetCurrentThreadId(), "layers change on their own thread: on the frame's each change is a whole apply pass");
    check(logged("LIGHTS OUT"), "lights out is logged");
    launch_all(40.f, 1.2);
    check(g_car[PLAYER].kmh < 50.f && g_car[PLAYER].ers.at<float>(ERS_DEPLOY) == 0.f, "the game's curve: nothing deployed below 50 km/h");
    check(!g_live[PLAYER].reached_50.load(), "not at 50 km/h yet");
    g_car[3].kmh = 30.f; // a car behind, so the run is still on when the player is past 50 km/h
    launch_all(40.f, 0.3);
    check(g_phase.load() == P_LAUNCH, "the run waits for the car behind");
    check(g_car[PLAYER].kmh >= 50.f && g_live[PLAYER].reached_50.load(), "the car has reached 50 km/h");
    check(g_live[PLAYER].deploy_above.load() > 0.f, "what the game deploys above 50 km/h is read back");
    float keep = g_car[PLAYER].kmh;
    g_car[PLAYER].kmh = 30.f;
    tick();
    tick();
    check(g_live[PLAYER].reached_50.load(), "it stays reached if the car slows down again");
    g_car[PLAYER].kmh = keep;
    launch_all(40.f, 4.0);
    check(g_phase.load() == P_NONE, "the run ends once every car is past 50 km/h with full boost");
    wait_layers();
    check(layer_count() == 0, "every layer is gone");
    check(logged("launch done: 0 low-output car(s); player car 5"), "the summary is logged");
    check(logged("launch done: AI cars at 50 km/h: 21 of 21, first "), "every AI car reached 50 km/h");
    check(logged("MGU-K peak 0 W below 50 km/h"), "the log shows the game's own cut");
    check(logged("Pre-Start over after") && logged("peak 11000 rpm"), "the Pre-Start line says what the revs did");
    check(g_routes == 1, "the game finished the lights once");
    check(g_car[PLAYER].ers.at<float>(ERS_MAX_DEPLOY) == ERS_2026, "the real value stays");
    uint32_t before = g_car[PLAYER].steps;
    tick();
    check(g_car[PLAYER].cap_seen == ERS_2026 && g_car[PLAYER].steps > before, "after the run the hook only passes through");
}

struct PanelLog {
    unsigned updates, ticks, shaders, models, blue, state;
    bool ok;
};
static PanelLog panel_log()
{
    PanelLog l{};
    std::string s = log_line("Pre-Start panels: ");
    size_t at = s.find("Pre-Start panels: ");
    if (at != std::string::npos)
        l.ok = sscanf_s(s.c_str() + at, "Pre-Start panels: %u zone updates in %u ticks, %u with shaders on, %u with a panel model, %u blue; session state 0x%x",
                        &l.updates, &l.ticks, &l.shaders, &l.models, &l.blue, &l.state) == 6;
    return l;
}

static void test_trackside_panels()
{
    reset("trackside panels");
    g_session->at<uint32_t>(SESSION_STATE) = 8;
    session_start("grand_prix", "race");
    fake_marshal(); // not a whole tick: the game's lights update only runs once the lights state is entered
    check(g_seen.calls == PANEL_ZONES && g_seen.game == PANEL_ZONES, "before the lights enter: the game's own values");
    enter();
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_PRE, "Pre-Start");
    g_seen = PanelsSeen{};
    // A whole flash cycle of real time: the flash runs on the overlay's clock.
    const uint32_t n = 50;
    for (uint32_t i = 0; i < n; i++) {
        tick();
        Sleep(15);
    }
    check(g_phase.load() == P_PRE, "still Pre-Start");
    check(g_seen.calls == n * PANEL_ZONES, "the game's zone update runs once per call of the wrap");
    check(g_seen.wrong_args == 0, "with the game's own arguments");
    check(g_seen.blue > 0 && g_seen.off > 0, "the update sends blue and off: a flash");
    check(g_seen.game == 0 && g_seen.other == 0, "and nothing else during Pre-Start");
    ticks(4.4);
    check(g_phase.load() == P_LIGHTS, "Pre-Start is over");
    PanelLog l = panel_log();
    check(l.ok, "the panels line is logged");
    check(l.ticks >= 300 && l.ticks <= 320, "about 5 s of ticks at 60 Hz");
    check(l.updates == l.ticks * PANEL_ZONES, "every zone, every tick");
    check(l.shaders == l.updates, "the system's shader switch is read");
    check(l.models == l.ticks * 2, "a zone without a model, or with one not in the world, does not count as a panel");
    check(l.blue > 0 && l.blue < l.updates, "the blue half is counted");
    check(l.state == 8, "the session state is in the line");
    g_seen = PanelsSeen{};
    ticks(0.5);
    check(g_seen.calls > 0 && g_seen.game == g_seen.calls, "red lights: the game's own values, with nothing to put back");
    run_to_lights_out(11000);
    g_seen = PanelsSeen{};
    tick();
    check(g_phase.load() == P_LAUNCH && g_seen.calls == PANEL_ZONES && g_seen.game == PANEL_ZONES, "lights out: the game's own values");
    launch_all(60.f, 6.0);
    check(g_phase.load() == P_NONE, "the start is over");
    g_seen = PanelsSeen{};
    tick();
    check(g_seen.calls == PANEL_ZONES && g_seen.game == PANEL_ZONES, "after the run: the game's own values");

    // A new start counts its own updates.
    g_sl->at<uint8_t>(SL_STATE) = 0;
    g_st->at<float>(ST_DELAY) = -0.01f;
    g_routed = false;
    for (uint32_t c = 0; c < RACERS; c++)
        g_car[c].kmh = 0;
    {
        std::lock_guard<std::mutex> g(g_log_lock);
        g_logs.clear();
    }
    enter();
    frame_cb(1.0 / 60.0, nullptr);
    check(g_zone_updates.load() == 0 && g_zone_ticks.load() == 0 && g_zone_shaders.load() == 0 && g_zone_models.load() == 0 && g_zone_blue.load() == 0 &&
              g_zone_first.load() == 0,
          "a new start counts from 0");
    ticks(5.2);
    l = panel_log();
    check(l.ok && l.ticks >= 280 && l.ticks <= 320, "the second start's line counts the second start");
}

static void test_trackside_panels_frozen()
{
    reset("trackside panels, frozen");
    session_start("grand_prix", "race");
    enter();
    tick();
    ticks(0.5);
    Sleep(int(FROZEN_MS) + 60);
    frame_cb(1.0 / 60.0, nullptr); // only frames: no game update and no physics
    check(g_frozen.load(), "frozen");
    g_seen = PanelsSeen{};
    fake_marshal();
    check(g_seen.game == PANEL_ZONES, "a frozen game keeps its own values, like the overlay");
    tick();
    g_seen = PanelsSeen{};
    fake_marshal();
    check(g_seen.game == 0 && g_seen.blue + g_seen.off == PANEL_ZONES, "the flash is back after the freeze");

    reset("trackside panels, paused");
    session_start("grand_prix", "race");
    enter();
    tick();
    ticks(0.5);
    g_session->at<uint32_t>(SESSION_STATE) = SESSION_PAUSED;
    tick();
    g_seen = PanelsSeen{};
    ticks(0.5);
    check(g_seen.calls == 0, "the game's system stands still in a pause");
    g_session->at<uint32_t>(SESSION_STATE) = 0;
    tick();
    g_seen = PanelsSeen{};
    fake_marshal();
    check(g_seen.game == 0 && g_seen.blue + g_seen.off == PANEL_ZONES, "the flash is back after the pause");
}

static void test_trackside_panels_not_on()
{
    reset("trackside panels, Pre-Start off");
    s_pre_start = false;
    session_start("grand_prix", "race");
    enter();
    ticks(1.0);
    check(g_phase.load() == P_LIGHTS, "the lights run");
    check(g_seen.calls > 0 && g_seen.game == g_seen.calls, "the setting off: the game's own values");
    check(!logged("Pre-Start panels: "), "and no line");

    reset("trackside panels, not F1 2026");
    set_grid(ERS_2025, HANDLING_2025);
    session_start("grand_prix", "race");
    enter();
    ticks(2.0);
    check(g_phase.load() == P_NONE, "no run");
    check(g_seen.calls > 0 && g_seen.game == g_seen.calls, "other cars: the game's own values");

    reset("trackside panels, not armed");
    session_start("online", "race");
    enter();
    ticks(2.0);
    check(g_phase.load() == P_NONE, "no run");
    check(g_seen.calls > 0 && g_seen.game == g_seen.calls, "online: the game's own values");

    reset("trackside panels, the game's own delay");
    session_start("grand_prix", "race");
    g_game_delay = 2.f;
    enter();
    tick();
    tick();
    ticks(1.5);
    check(g_seen.calls > 0 && g_seen.game == g_seen.calls, "the game's own delay stays the game's");
    ticks(0.7);
    g_seen = PanelsSeen{};
    tick();
    tick();
    check(g_seen.calls == 2 * PANEL_ZONES && g_seen.blue + g_seen.off == g_seen.calls, "the last 5 s are Pre-Start");

    reset("trackside panels, shaders switched off");
    g_marshal->at<uint8_t>(MARSHAL_SHADERS) = 0;
    session_start("grand_prix", "race");
    enter();
    ticks(5.2);
    PanelLog l = panel_log();
    check(l.ok && l.updates > 0 && l.shaders == 0 && l.models == 0, "the line tells the game's switch from a missing panel");
}

static void test_formation_lap_leftover()
{
    reset("start after a formation lap");
    session_start("driver_career", "race", "race_2");
    // The formation's one light left the shared lights object in mode 2, state 4.
    g_sl->at<uint8_t>(SL_MODE) = SL_MODE_FORMATION;
    g_sl->at<uint8_t>(SL_STATE) = 4;
    enter();
    check(std::fabs(g_st->at<float>(ST_DELAY) - 5.f) < 1e-4f, "5 s added");
    tick();
    check(!g_routed, "the leftover state must not finish the start");
    check(g_phase.load() == P_PRE, "Pre-Start, not lights out");
    ticks(4.5);
    check(!g_routed && g_phase.load() == P_PRE, "still waiting after 4.5 s");
    check(layer_count() == 0, "no launch power during the wait");
    ticks(0.6);
    check(g_sl->at<uint8_t>(SL_STATE) == 1 && g_sl->at<uint8_t>(SL_MODE) == 0, "the game starts the five lights");
    run_to_lights_out(11000);
    check(g_lights_t >= 5.f + g_hold_s - 0.1f, "lights out came after the whole sequence");
}

static void test_formation_lap_leftover_no_pre_start()
{
    reset("formation lap leftover, Pre-Start off");
    session_start("driver_career", "race");
    s_pre_start = false;
    g_sl->at<uint8_t>(SL_MODE) = SL_MODE_FORMATION;
    g_sl->at<uint8_t>(SL_STATE) = 4;
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f, "no delay");
    frame_cb(1.0 / 60.0, nullptr); // a frame before the game's first update
    check(g_phase.load() == P_LIGHTS, "waits for the lights, no false lights out");
    tick();
    check(g_sl->at<uint8_t>(SL_STATE) == 1, "the game starts the lights in its first update");
    run_to_lights_out(11000);
}

static void test_game_delay()
{
    reset("the game has its own delay");
    session_start("grand_prix", "race");
    g_game_delay = 2.f;
    enter();
    check(std::fabs(g_st->at<float>(ST_DELAY) - 7.f) < 1e-4f, "5 s on top");
    tick();
    tick();
    check(!lights_on_screen() && g_leaf_writes == 0, "the first 2 s are the game's");
    check(painted("TURBO SPOOL"), "the spool gauge shows anyway");
    ticks(2.2);
    check(lights_on_screen() && lap_says_pre_start(), "the last 5 s are Pre-Start");
}

// An armed race whose cars are not F1 2026 cars: the game's own start, and nothing else.
static void expect_standard_start(const char *why)
{
    session_start("grand_prix", "race");
    check(g_offline.load(), "the session alone would arm");
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f, why);
    check(g_pending_st.load() == 0, "no run is handed to the frame");
    check(logged("standard start, not F1 2026 cars"), "logged");
    tick();
    check(g_phase.load() == P_NONE, "no run");
    paint();
    check(g_drawn.empty() && g_leaf_writes == 0, "nothing on screen, nothing in the game's HUD");
    ticks(9.0);
    check(g_sl->at<uint8_t>(SL_STATE) >= 3, "the game's own lights run");
    check(g_phase.load() == P_NONE && layer_count() == 0, "no launch power");
}

static void test_only_f1_2026()
{
    reset("2025 cars");
    set_grid(ERS_2025, HANDLING_2025);
    expect_standard_start("no Pre-Start on 2025 cars");
    check(logged("23 cars, 0 F1 2026, 22 others with an ERS (handling version 8, max deploy 126300 W)"), "the grid is logged");

    reset("2026 power on 2025 handling");
    set_grid(ERS_2026, HANDLING_2025);
    expect_standard_start("the handling version counts");

    reset("2026 handling with 2025 power");
    set_grid(ERS_2025, HANDLING_2026);
    expect_standard_start("the power counts");

    reset("F2 cars: no ERS");
    for (uint32_t i = 0; i <= RACERS; i++) {
        g_car[i].no_ers = true;
        g_car[i].car.at<uintptr_t>(CAR_ERS) = 0;
        g_car[i].car.at<uint32_t>(CAR_HANDLING) = HANDLING_2025;
    }
    expect_standard_start("no ERS on the grid");
    check(logged("23 cars, 0 F1 2026, 0 others"), "the grid is logged");

    reset("one car of another spec");
    set_car(3, ERS_2025, HANDLING_2025);
    expect_standard_start("a mixed grid");
    check(logged("23 cars, 21 F1 2026, 1 others"), "the grid is logged");

    reset("the car table is not available");
    g_table_down = true;
    expect_standard_start("no cars read, no start changed");
    check(logged("0 cars, 0 F1 2026, 0 others"), "the grid is logged");

    reset("no car operations");
    g_cars = nullptr;
    expect_standard_start("no cars read, no start changed");

    reset("a formation leftover on older cars");
    set_grid(ERS_2025, HANDLING_2025);
    session_start("driver_career", "race");
    g_sl->at<uint8_t>(SL_MODE) = SL_MODE_FORMATION;
    g_sl->at<uint8_t>(SL_STATE) = 4;
    enter();
    check(g_sl->at<uint8_t>(SL_STATE) == 4 && g_st->at<float>(ST_DELAY) == 0.f, "the lights object stays the game's");

    reset("every start asks again");
    session_start("driver_career", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    launch_all(60.f, 6.0);
    check(g_phase.load() == P_NONE, "the first start ran");
    set_grid(ERS_2025, HANDLING_2025);
    g_sl->at<uint8_t>(SL_STATE) = 0;
    g_st->at<float>(ST_DELAY) = -0.01f;
    g_routed = false;
    enter();
    check(g_st->at<float>(ST_DELAY) < 0.f && g_pending_st.load() == 0, "the next start with other cars is the game's");

    reset("the safety car and a car the table holds back do not stop a 2026 grid");
    g_table_late = 3;
    session_start("grand_prix", "race");
    enter();
    check(g_st->at<float>(ST_DELAY) > 0.f, "Pre-Start");
    check(logged("22 cars, 21 F1 2026, 0 others"), "the grid is logged");

    reset("a car whose ERS record is not its own");
    set_car(3, ERS_2025, HANDLING_2025);
    g_car[3].ers.at<uintptr_t>(SYS_OWNER) = g_car[4].car.p();
    session_start("grand_prix", "race");
    enter();
    check(g_st->at<float>(ST_DELAY) > 0.f, "Pre-Start: the record says nothing about this car");
    check(logged("23 cars, 21 F1 2026, 0 others"), "not counted");

    reset("a car that is already gone");
    auto *gone = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    VirtualFree(gone, 0, MEM_RELEASE);
    g_table_freed = reinterpret_cast<uintptr_t>(gone);
    long faults = g_faults.load();
    session_start("grand_prix", "race");
    enter();
    check(g_faults.load() == faults, "no fault for the core's crash handler to report");
    check(logged("23 cars, 21 F1 2026, 0 others"), "the others still count");

    reset("a car without the 2026 power at lights out");
    session_start("grand_prix", "race");
    enter();
    set_car(3, ERS_2025, HANDLING_2025);
    tick();
    run_to_lights_out(4000);
    wait_layers();
    check(!g_run[3].active && !has_layer(3), "no launch model on it");
    check(g_run[4].active && g_run[PLAYER].active, "the others have it");
}

static void test_no_revs()
{
    reset("no revs before lights out");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(4000);
    wait_layers();
    check(std::fabs(layered_power(PLAYER) - 0.35f) < 0.001f, "no boost: 35 %");
    check(g_player_boost.load() == 0.f, "the bar stays empty");
    g_car[PLAYER].rpm = 8000;
    float b = g_run[PLAYER].boost;
    ticks(0.5);
    check(g_run[PLAYER].boost == b, "low revs after lights out neither build nor lose boost");
    g_car[PLAYER].rpm = 11000;
    ticks(0.5);
    check(g_run[PLAYER].boost > 0.1f, "high revs build it");
}

static void test_rolling_before_lights_out()
{
    reset("a car still rolling into its slot");
    session_start("driver_career", "race");
    enter();
    tick();
    g_car[3].kmh = 60.f;
    ticks(1.0);
    check(!g_live[3].reached_50.load(), "not counted while rolling");
    g_car[3].kmh = 0.f;
    run_to_lights_out(11000);
    tick();
    tick();
    check(!g_live[3].reached_50.load(), "60 km/h before the start does not count as reaching 50 km/h");
    launch_all(60.f, 1.2);
    check(g_live[3].reached_50.load(), "the launch's own 50 km/h does");
}

static void test_revs_fall_at_launch()
{
    reset("the revs fall when the clutch bites");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    float b = g_run[PLAYER].boost;
    check(b > 0.99f, "full boost at lights out");
    g_car[PLAYER].rpm = 6000;
    ticks(1.0);
    check(g_run[PLAYER].boost == b, "the launch does not cost boost");
}

static void test_physics_clock()
{
    reset("slow motion: clocks are physics time");
    session_start("grand_prix", "race");
    enter();
    tick();
    g_car[PLAYER].rpm = 11000;
    auto half_speed = [](double frames_s) {
        for (int i = 0, n = int(frames_s * 60.0); i < n && g_phase.load() != P_NONE; i++) {
            fake_update(1.0 / 120.0);
            run_steps(1.0 / 120.0, true);
            frame_cb(1.0 / 60.0, nullptr);
        }
    };
    half_speed(3.0);
    check(g_player_boost.load() > 0.45f && g_player_boost.load() < 0.55f, "3 s of frames are 1.5 s of spool");
    half_speed(21.0);
    check(g_phase.load() == P_LAUNCH, "lights out");
    for (uint32_t c = 0; c < RACERS; c++)
        g_car[c].kmh = 20.f;
    float at = g_launch_s;
    half_speed(13.0);
    check(g_phase.load() == P_LAUNCH, "13 s of frames are 6.5 s of the 12 s window");
    check(g_launch_s - at > 6.0f && g_launch_s - at < 7.0f, "the launch clock");
    half_speed(13.0);
    check(g_phase.load() == P_NONE, "the window closes at 12 s of physics time");
}

static void test_boost_decays_while_waiting()
{
    reset("revs dropped before lights out");
    session_start("grand_prix", "race");
    enter();
    tick();
    g_car[PLAYER].rpm = 11000;
    ticks(3.2);
    check(g_player_boost.load() > 0.99f, "full boost");
    g_car[PLAYER].rpm = 6000;
    ticks(1.5);
    check(g_player_boost.load() > 0.70f && g_player_boost.load() < 0.80f, "1.5 s below the revs: down a quarter");
}

static void test_pause_in_pre_start()
{
    reset("pause in Pre-Start");
    session_start("grand_prix", "race");
    enter();
    tick();
    g_car[PLAYER].rpm = 11000;
    ticks(1.0);
    float delay = g_st->at<float>(ST_DELAY), boost = g_player_boost.load();
    double waited = g_wait_s;
    g_session->at<uint32_t>(SESSION_STATE) = SESSION_PAUSED;
    tick();
    check(!lights_on_screen(), "hidden in the pause menu");
    check(!painted("TURBO SPOOL"), "the spool gauge too");
    int writes = g_leaf_writes;
    for (int i = 0; i < 120; i++)
        tick();
    check(g_st->at<float>(ST_DELAY) == delay, "the game's countdown stops");
    check(g_player_boost.load() == boost, "no boost while paused");
    check(g_wait_s == waited, "the timeout clock stops");
    check(g_phase.load() == P_PRE, "the run waits");
    check(lap_says_pre_start() && !logged("lap counter back"), "the lap counter keeps PRE-START: the pause menu hides the HUD");
    check(g_leaf_writes - writes <= 8, "at most every half second while paused");
    g_session->at<uint32_t>(SESSION_STATE) = 0;
    tick();
    check(lights_on_screen(), "back after the pause");
    ticks(4.2);
    check(g_sl->at<uint8_t>(SL_STATE) == 1, "the 5 s of Pre-Start are played in full");
}

static void test_freeze_without_pause_state()
{
    reset("a freeze the session state does not show");
    session_start("grand_prix", "race");
    enter();
    tick();
    ticks(0.5);
    check(lights_on_screen(), "showing");
    Sleep(int(FROZEN_MS) + 60);
    frame_cb(1.0 / 60.0, nullptr); // only frames: no game update and no physics
    check(!lights_on_screen(), "hidden: no physics time and no countdown");
    tick();
    check(lights_on_screen(), "back");
}

static void test_reenter()
{
    reset("the enter runs again");
    session_start("grand_prix", "race");
    enter();
    tick();
    ticks(2.0);
    float delay = g_st->at<float>(ST_DELAY);
    enter();
    check(g_st->at<float>(ST_DELAY) == delay, "in Pre-Start: no second 5 s");
    check(g_pending_st.load() == 0, "no second run");
    ticks(3.2);
    check(g_sl->at<uint8_t>(SL_STATE) == 1, "lights running");
    enter();
    check(g_st->at<float>(ST_DELAY) <= 0.f, "while the lights run: nothing");
    check(g_pending_st.load() == 0, "no new run");
    run_to_lights_out(11000);
    tick();
    enter();
    check(g_pending_st.load() == 0, "after lights out: no new run");
    check(g_phase.load() == P_LAUNCH, "the launch goes on");
}

static void test_restart_after_race()
{
    reset("race restart from the results screen");
    session_start("driver_career", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    launch_all(60.f, 6.0);
    check(g_phase.load() == P_NONE, "the first start is over");
    // No session events. The game has reset its lights; the state object and its first-run flag stay.
    g_sl->at<uint8_t>(SL_STATE) = 0;
    g_st->at<float>(ST_DELAY) = -0.01f;
    g_routed = false;
    for (uint32_t c = 0; c < RACERS; c++)
        g_car[c].kmh = 0;
    enter();
    check(std::fabs(g_st->at<float>(ST_DELAY) - 5.f) < 1e-4f, "5 s again");
    tick();
    check(g_phase.load() == P_PRE, "a new run");
    tick();
    check(!g_live[PLAYER].reached_50.load(), "50 km/h counts again for the new start");
}

static void test_restart_during_launch()
{
    reset("restart during the launch");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(4000);
    wait_layers();
    check(has_layer(PLAYER), "layers on");
    g_sl->at<uint8_t>(SL_STATE) = 0;
    g_st->at<float>(ST_DELAY) = 0;
    g_routed = false;
    enter();
    tick();
    check(g_phase.load() == P_PRE, "a new run");
    wait_layers();
    check(layer_count() == 0, "the old launch power is gone");
}

static void test_session_end_during_launch()
{
    reset("session ends during the launch");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    wait_layers();
    fire(SWOFTY_EVENT_SESSION_END);
    tick(1.0 / 60.0, false);
    check(g_phase.load() == P_NONE, "the run is over");
    wait_layers();
    check(layer_count() == 0, "layers gone");
    check(g_car[PLAYER].ers.at<float>(ERS_MAX_DEPLOY) == ERS_2026, "max deploy untouched");
    check(g_offline.load(), "a session end keeps the arming");
    fire(SWOFTY_EVENT_SESSION_UNLOAD);
    check(!g_offline.load() && !g_race_label.load(), "an unload clears it");

    reset("unload before the lights");
    session_start("grand_prix", "race");
    fire(SWOFTY_EVENT_SESSION_UNLOAD);
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f && g_pending_st.load() == 0, "no start before the next session.start");
    check(logged("standard start, not an offline mode (kind 0, type 15)"), "logged as not offline, with the game's type");
}

// A Driver Career weekend played in one sitting, as a real log shows it: the core's session label stayed practice_1
// from the first practice to the race, while the game's own session type at the lights said 15.
static void test_career_weekend()
{
    reset("career weekend in one sitting");
    for (int i = 0; i < 4; i++) { // practice 1, practice 3, qualifying, qualifying again: no lights in any of them
        session_start("driver_career", "practice", "practice_1");
        check(logged("-> offline mode, the lights check the session type and the cars"), "each session.start logs the mode");
        fire(SWOFTY_EVENT_SESSION_END);
    }
    session_start("driver_career", "practice", "practice_1");
    g_desc->at<uint32_t>(4) = 15;
    enter();
    check(std::fabs(g_st->at<float>(ST_DELAY) - 5.f) < 1e-4f, "the race gets its 5 s of Pre-Start");
    check(logged("StartLights enter: session kind 0, type 15, first run 1"), "logged with the game's type");
    tick();
    check(g_phase.load() == P_PRE, "Pre-Start");
    tick();
    check(lights_on_screen() && lap_says_pre_start() && painted("TURBO SPOOL"), "the HUD shows");
    run_to_lights_out(11000);
    check(g_player_boost.load() > 0.99f, "full boost at lights out");

    reset("career qualifying with a race label");
    session_start("driver_career", "race", "race");
    g_desc->at<uint32_t>(4) = 5;
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f && g_pending_st.load() == 0, "a label stuck at an earlier race does not arm qualifying");
    check(logged("standard start, session type 5 is not a race (kind 0)"), "the game's type vetoes it");
}

static void test_gates()
{
    // mode and label from session.start, type from the game's own session descriptor at the lights
    struct Case {
        const char *mode, *label;
        uint32_t type;
        bool armed;
    } cases[] = {
        {"grand_prix", "race", 15, true},
        {"driver_career", "race", 16, true},
        {"my_team", "race", 17, true},
        {"driver_career", "sprint", 15, true},
        // The core's label stays at the weekend's first session: the game's type decides.
        {"driver_career", "practice", 15, true},
        {"my_team", "qualifying", 16, true},
        {"grand_prix", "practice", 17, true},
        {"driver_career", "race", 3, false},
        {"driver_career", "sprint", 14, false},
        {"grand_prix", "race", 18, false},
        {"my_team", "race", 1, false},
        // Outside the game's table the label decides.
        {"driver_career", "practice", 0, false},
        {"driver_career", "qualifying", 0x41414141, false},
        {"grand_prix", "race", 0, true},
        {"my_team", "sprint", 19, true},
        // Online and every other mode never.
        {"unknown", "race", 15, false},
        {"unknown", "practice", 16, false},
        {"story", "race", 15, false},
        {"split_screen", "race", 15, false},
        {"f1_world", "race", 16, false},
        {"time_trial", "time_trial", 18, false},
    };
    for (const Case &c : cases) {
        reset("session gate");
        session_start(c.mode, c.label);
        g_desc->at<uint32_t>(4) = c.type;
        g_sl->at<uint8_t>(SL_STATE) = 0;
        enter();
        check((g_st->at<float>(ST_DELAY) > 0.f) == c.armed, c.label);
        tick();
        check((g_phase.load() != P_NONE) == c.armed, c.mode);
        if (!c.armed) {
            run_steps(1.0 / 60.0, true);
            check(g_car[PLAYER].cap_seen == ERS_2026, "the MGU-K is not touched");
            ticks(9.0);
            check(g_sl->at<uint8_t>(SL_STATE) >= 3, "the game's own lights run");
            check(g_phase.load() == P_NONE && layer_count() == 0, "nothing else happens");
        }
    }
    reset("descriptor veto");
    session_start("grand_prix", "race");
    g_desc->at<uint32_t>(4) = 3;
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f, "the game's own session type says practice");
    reset("descriptor sprint");
    session_start("grand_prix", "sprint");
    g_desc->at<uint32_t>(4) = 16;
    enter();
    check(g_st->at<float>(ST_DELAY) > 0.f, "race_2 passes");
    reset("descriptor unreadable");
    session_start("grand_prix", "race");
    g_desc->at<uint32_t>(4) = 0x41414141;
    enter();
    check(g_st->at<float>(ST_DELAY) > 0.f, "a value outside the table is no veto");
    reset("lights disabled");
    session_start("grand_prix", "race");
    g_sl->at<uint8_t>(SL_ENABLED) = 0;
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f, "no lights, no Pre-Start");
    reset("lights already over");
    session_start("grand_prix", "race");
    g_sl->at<uint8_t>(SL_STATE) = 4;
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f, "state 4 in mode 0 is not touched");
    check(g_sl->at<uint8_t>(SL_STATE) == 4, "the game's state stays");
    tick();
    check(g_routed && g_phase.load() == P_NONE, "the game moves on as it would");
}

static void test_settings()
{
    reset("launch model off");
    session_start("grand_prix", "race");
    s_launch = false;
    enter();
    check(std::fabs(g_st->at<float>(ST_DELAY) - 5.f) < 1e-4f, "Pre-Start stays");
    tick();
    tick();
    check(!painted("TURBO SPOOL"), "no spool gauge");
    check(lights_on_screen() && lap_says_pre_start(), "Pre-Start shows");
    for (int i = 0; i < 60 * 15 && !g_routed; i++)
        tick();
    tick();
    check(g_phase.load() == P_NONE, "the run ends at lights out");
    wait_layers();
    check(layer_count() == 0, "no layers");

    reset("Pre-Start off");
    session_start("grand_prix", "race");
    s_pre_start = false;
    enter();
    check(g_st->at<float>(ST_DELAY) == 0.f, "no delay");
    tick();
    check(g_phase.load() == P_LIGHTS, "launch model still runs");
    check(!lights_on_screen() && g_leaf_writes == 0, "no blue lights, nothing in the lap counter");
    run_to_lights_out(11000);

    reset("both off");
    session_start("grand_prix", "race");
    s_pre_start = false;
    s_launch = false;
    enter();
    tick();
    check(g_phase.load() == P_NONE, "the mod does nothing");

    reset("AI exempt");
    session_start("grand_prix", "race");
    s_ai = false;
    enter();
    tick();
    tick();
    run_to_lights_out(4000);
    wait_layers();
    for (uint32_t c = 0; c < RACERS; c++)
        check(c == PLAYER || !has_layer(c), "AI engine untouched");
    check(layered_power(PLAYER) < 1.f, "player engine scaled");

    reset("sliders");
    g_settings = {{"spool_seconds", 1.0}, {"spool_rpm", 12000.0}, {"safety_net", 0.0}};
    read_settings();
    check(s_spool.load() == 1.f && s_spool_rpm.load() == 12000.f && !s_net.load(), "read as the manifest gives them");
    session_start("grand_prix", "race");
    enter();
    tick();
    check(logged("launch model on, safety net off, AI on, spool 1.0 s at 12000 rpm"), "each start logs the settings it runs with");
    g_car[PLAYER].rpm = 11000;
    ticks(2.0);
    check(g_player_boost.load() == 0.f, "11000 rpm is below a 12000 rpm slider");
    g_car[PLAYER].rpm = 12500;
    ticks(1.1);
    check(g_player_boost.load() > 0.99f, "1 s to spool");
    run_to_lights_out(12500);
    wait_layers();
    check(!has_layer(PLAYER), "full boost at the slider's revs");
}

static float deploy_of(uint32_t c) { return g_car[c].ers.at<float>(ERS_DEPLOY); }
static float assist_of(uint32_t c) { return g_live[c].assist.load(); }

// To lights out, then the given car rolling at 5 km/h with no boost, the others away at 40 km/h with full boost.
static void net_start(uint32_t slow, float rpm = 4000)
{
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(rpm);
    for (uint32_t c = 0; c < RACERS; c++) {
        g_run[c].boost = c == slow ? 0.f : 1.f;
        g_car[c].kmh = c == slow ? 5.f : 40.f;
    }
}

static void test_safety_net()
{
    reset("safety net, the player");
    net_start(PLAYER);
    g_run[4].boost = 0.f; // holds 40 km/h: fast, but gaining nothing
    ticks(0.9);
    check(!logged("low-output start"), "not before the check");
    check(deploy_of(PLAYER) == 0.f && assist_of(PLAYER) == 0.f, "no MGU-K before the check");
    wait_layers();
    check(layered_power(PLAYER) < 0.4f, "a missed spool is about a third of the power");
    ticks(0.2);
    check(logged("low-output start: car 5") && logged("MGU-K on"), "detected and logged");
    check(g_run[PLAYER].flagged && !g_run[3].flagged, "only this car");
    check(g_run[4].flagged, "what a car gained counts, not the speed it has");
    float early = assist_of(PLAYER);
    check(early > 0.f && early < 0.4f, "it starts small: the line starts at the car's own speed");
    tick();
    check(std::fabs(deploy_of(PLAYER) - early * ERS_2026) < 1.f, "the game's own deploy code gives what the curve answers");
    check(g_car[PLAYER].cap_seen == ERS_2026, "the max deploy stays the game's");
    check(deploy_of(3) == 0.f, "a car the net did not flag keeps the game's 0");
    ticks(0.4);
    check(std::fabs(assist_of(PLAYER) - 0.5f) < 0.001f, "the line stays within 1 m/s of a car that gains nothing");
    check(std::fabs(deploy_of(PLAYER) - 0.5f * ERS_2026) < 1.f, "half the max deploy at that gap");
    g_car[PLAYER].deploy_wanted = 150e3f; // the Medium mode
    tick();
    check(deploy_of(PLAYER) == 150e3f, "the ERS mode's limit stays in force");
    g_car[PLAYER].deploy_wanted = 350e3f;
    wait_layers();
    check(layered_power(PLAYER) < 0.4f, "the net does not touch the engine");
    check(toasts_of(MSG_LOW) == 1, "the game's message box shows LOW-OUTPUT START");
    check(!g_toasts.empty() && g_toasts[0].detail == MSG_LOW_SUB && g_toasts[0].seconds == NET_FLASH_S && g_toasts[0].icon.empty(),
          "with its second line, for 5 s, the default icon");
    check(!painted(MSG_LOW), "the overlay does not draw it as well");
    check(toasts_of(MSG_AHEAD) == 0, "no SLOW CAR AHEAD with the player's own");
    float line = g_run[PLAYER].net_line;
    g_car[PLAYER].kmh = (line + 3.f) * 3.6f;
    tick();
    tick();
    check(assist_of(PLAYER) == 0.f && deploy_of(PLAYER) == 0.f, "above its minimum acceleration: nothing");
    g_car[PLAYER].kmh = 0.f;
    tick();
    tick();
    check(assist_of(PLAYER) == 0.f && deploy_of(PLAYER) == 0.f, "a car that stopped gets nothing");
    g_car[PLAYER].kmh = 5.f;
    ticks(0.5);
    check(assist_of(PLAYER) > 0.f && assist_of(PLAYER) <= 0.5f, "moving again: no burst for the time it stood");
    // The steps alone, before any frame: 50 km/h ends it at once.
    g_car[PLAYER].kmh = 60.f;
    run_steps(1.0 / 60.0, true);
    check(assist_of(PLAYER) > 0.f, "the frame has not run yet");
    check(g_live[PLAYER].reached_50.load(), "the car has reached 50 km/h");
    check(std::fabs(deploy_of(PLAYER) - deploy_curve(60.f / 3.6f) * ERS_2026) < 1.f, "the game's own curve from 50 km/h on");
    g_run[PLAYER].net_line = 30.f; // ahead of the car, so its speed alone would not end the help
    tick();
    check(assist_of(PLAYER) == 0.f, "the frame sees it too");
    float below = g_live[PLAYER].deploy_below.load();
    check(below >= 175e3f - 1.f && below <= 350e3f, "the peak below 50 km/h is read back");
    g_car[PLAYER].rpm = 11000;
    g_car[4].kmh = 60.f;
    launch_all(40.f, 4.0);
    check(g_phase.load() == P_NONE, "the run ends");
    check(toasts_of(MSG_LOW) == 1 && toasts_of(MSG_AHEAD) == 0, "one message in the whole run");
    check(logged("launch done: low-output car 5: 50 km/h after ") && logged("low-output car 4: "), "each flagged car's summary is logged");
    check(logged("launch done: 2 low-output car(s)"), "the count");

    reset("safety net, a slow car ahead");
    net_start(3, 11000);
    g_car[3].z = 50.f;
    g_car[PLAYER].z = 0.f;
    ticks(1.1);
    ticks(0.1);
    check(logged("low-output start: car 3"), "detected");
    check(g_ahead_flag.load(), "it is ahead");
    check(toasts_of(MSG_AHEAD) == 1 && toasts_of(MSG_LOW) == 0, "the game's message box shows SLOW CAR AHEAD");
    check(logged("game message SLOW CAR AHEAD queued"), "logged");
    check(!painted(MSG_AHEAD), "the overlay does not draw it as well");
    check(assist_of(3) > 0.f && deploy_of(3) > 0.f, "the AI gets the same help");
    check(g_live[3].deploy_below.load() > 0.f, "and its peak is read back");

    reset("safety net, a stationary car");
    net_start(3, 11000);
    g_car[3].kmh = 0.f;
    ticks(1.2);
    check(!logged("low-output start: car 3"), "a stalled car is left alone");
    check(assist_of(3) == 0.f && deploy_of(3) == 0.f, "and gets nothing");

    reset("safety net, a late reaction");
    net_start(PLAYER);
    g_car[PLAYER].kmh = 0.f;
    ticks(0.9);
    for (int i = 0; i < 80; i++) {
        g_car[PLAYER].kmh += 6.f * 3.6f / 60.f;
        tick();
    }
    check(!g_run[PLAYER].flagged, "0.9 s late, then a fair launch: the check counts from when it moved");

    reset("safety net, a late and slow start");
    net_start(PLAYER);
    g_car[PLAYER].kmh = 0.f;
    ticks(2.0);
    for (int i = 0; i < 80; i++) {
        g_car[PLAYER].kmh += 2.f * 3.6f / 60.f;
        tick();
    }
    check(g_run[PLAYER].flagged, "2 s late and low on power: the check is still to come when it moves");

    reset("safety net, low grip");
    net_start(PLAYER, 11000);
    g_run[PLAYER].boost = 1.f;
    ticks(1.3);
    check(!g_run[PLAYER].flagged, "full boost and no acceleration: wheelspin, not low power");
    check(assist_of(PLAYER) == 0.f && deploy_of(PLAYER) == 0.f, "no MGU-K");

    reset("safety net off");
    s_net = false;
    net_start(PLAYER);
    ticks(1.2);
    check(!logged("low-output start"), "no detection");
    check(assist_of(PLAYER) == 0.f && deploy_of(PLAYER) == 0.f, "no MGU-K");

    reset("safety net, the second curve");
    net_start(PLAYER);
    g_car[PLAYER].ers.at<uint8_t>(ERS_CURVE_FLAG) = 1;
    ticks(1.3);
    check(g_run[PLAYER].flagged && deploy_of(PLAYER) > 0.f, "the game reads the second curve, so that one answers");

    reset("safety net, evenly spaced samples");
    load_uniform_curve(g_car[PLAYER]);
    net_start(PLAYER);
    float before = fake_eval(g_car[PLAYER].ers, ERS_CURVE, 25.f);
    check(std::fabs(before - 0.295f) < 0.001f, "the fake's curve");
    ticks(1.3);
    check(g_run[PLAYER].flagged && deploy_of(PLAYER) > 0.f, "the same override works on this form");
    check(fake_eval(g_car[PLAYER].ers, ERS_CURVE, 25.f) == before, "and the curve is back after the step");

    reset("safety net, the start is run again with a car flagged");
    net_start(PLAYER);
    ticks(1.3);
    check(assist_of(PLAYER) > 0.f, "flagged and helped");
    g_car[PLAYER].kmh = 0.f;
    g_sl->at<uint8_t>(SL_STATE) = 0;
    g_st->at<float>(ST_DELAY) = 0;
    g_routed = false;
    enter();
    tick();
    check(g_phase.load() == P_PRE, "a new run");
    tick();
    check(assist_of(PLAYER) == 0.f && deploy_of(PLAYER) == 0.f, "no MGU-K left over on the grid");

    reset("safety net, a record that is not the car's");
    net_start(PLAYER);
    ticks(1.3);
    check(deploy_of(PLAYER) > 0.f, "flagged and helped");
    g_car[PLAYER].ers.at<uintptr_t>(SYS_OWNER) = g_car[4].car.p();
    tick();
    check(deploy_of(PLAYER) == 0.f, "no write into a record that is not the car's");
}

// A launch in the numbers the real logs give: 9 m/s^2 at full engine power. The MGU-K's 8 m/s^2 for the whole max deploy
// comes from the torque it adds at launch revs, and only a game log can show how close that is.
// What a launch gains per second, by engine power, from the real logs: 35 % 4.17 m/s^2 (the log of 2026-11-07), 55 % about
// 5, 77.5 % about 8, 88 % and up about 9, the traction limit.
static float launch_accel(float power)
{
    static const float at[][2] = {{0.35f, 4.17f}, {0.55f, 5.f}, {0.775f, 8.f}, {0.88f, 9.f}};
    if (power <= at[0][0])
        return at[0][1];
    for (int i = 1; i < 4; i++)
        if (power <= at[i][0])
            return at[i - 1][1] + (at[i][1] - at[i - 1][1]) * (power - at[i - 1][0]) / (at[i][0] - at[i - 1][0]);
    return 9.f;
}
constexpr float AI_LAUNCH = 0.8f;   // the same log: the AI's median reaches 50 km/h about 1.95 s after moving, where 9 m/s^2 takes 1.54 s
constexpr float MGUK_EFFECT = 3.6f; // m/s^2 for the whole max deploy. The same log: 32 kW closed the 0.33 m/s^2 between 4.17 and the net's 4.5

static float to_50_kmh(float boost, bool net, float *peak = nullptr)
{
    reset("the safety net in a closed loop");
    s_net = net;
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(4000);
    for (uint32_t c = 0; c < RACERS; c++) {
        g_run[c].boost = 1.f;
        g_car[c].kmh = c == PLAYER ? 0.f : 60.f;
    }
    g_run[PLAYER].boost = boost;
    g_run[PLAYER].power = gs::target_power(boost, tuning());
    double t = 0, dt = 1.0 / 60.0;
    for (; g_car[PLAYER].kmh < 50.f && t < 11.0; t += dt) {
        g_car[PLAYER].kmh += (launch_accel(g_run[PLAYER].power) + MGUK_EFFECT * deploy_of(PLAYER) / ERS_2026) * float(dt) * 3.6f;
        tick(dt);
    }
    if (peak)
        *peak = g_live[PLAYER].deploy_below.load();
    return float(t);
}

static void test_net_closed_loop()
{
    float peak = 0;
    float alone = to_50_kmh(0.f, false, &peak);
    check(alone > 3.2f && alone < 3.45f, "a missed spool with no net: 4.17 m/s^2, as in the game log");
    check(peak == 0.f, "and no MGU-K");
    float missed = to_50_kmh(0.f, true, &peak);
    printf("flow_test: closed loop, missed spool: %.2f s with no net, %.2f s with it, peak %.0f W\n", alone, missed, peak);
    check(missed > 3.0f && missed < alone - 0.1f, "the net makes a disaster a bad start");
    check(peak > 20e3f && peak < 45e3f, "with just enough MGU-K, as in the game log");
    float worse = missed;
    for (float boost : {0.05f, 0.1f, 0.15f, 0.2f, 0.25f, 0.3f, 0.5f}) {
        float time = to_50_kmh(boost, true);
        check(time <= worse + 0.02f, "a better spool is never the slower start: the net gives no advantage");
        worse = time;
    }
    float clean = to_50_kmh(1.f, true);
    check(clean > 1.45f && clean < 1.65f, "a clean start: 9 m/s^2");
    check(!g_run[PLAYER].flagged, "and no net");
}

// Sets an AI car up as begin_launch does.
static void set_ai_start(uint32_t c, float boost, bool hold = true)
{
    g_run[c].boost = boost;
    g_run[c].hold_s = hold ? gs::ai_refill_after(boost, tuning()) : 0.f;
    g_run[c].power = gs::target_power(boost, tuning());
}

static void test_ai_fumble()
{
    reset("a fumbled AI car bogs first");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    constexpr uint32_t FUMBLED = 2, EMPTY = 3, POOR = 4, GOOD = 5;
    for (uint32_t c = 0; c < RACERS; c++)
        if (c != PLAYER)
            set_ai_start(c, 0.8f);
    set_ai_start(FUMBLED, 0.1f);
    set_ai_start(EMPTY, 0.f);
    set_ai_start(POOR, 0.3f);
    set_ai_start(GOOD, 0.6f);
    auto near_boost = [](uint32_t c, float want) { return std::fabs(g_run[c].boost - want) < 0.02f; };
    // The AI cars sit at 4000 rpm here: their revs must not matter, in either direction.
    launch_all(20.f, 1.0);
    check(g_run[FUMBLED].boost == 0.1f && g_run[EMPTY].boost == 0.f, "1 s after lights out it has what it started with");
    check(near_boost(POOR, 0.3f + 1.f / 3.f), "a poor car refills at once, at any revs");
    check(near_boost(GOOD, 0.6f + 1.f / 3.f), "and a good one");
    wait_layers();
    check(std::fabs(layered_power(FUMBLED) - gs::target_power(0.1f, tuning())) < 0.01f, "the engine stays down with it");
    launch_all(20.f, 0.4);
    check(g_run[FUMBLED].boost == 0.1f && g_run[EMPTY].boost == 0.f, "at 1.4 s still");
    launch_all(20.f, 0.2);
    check(g_run[FUMBLED].boost > 0.1f && g_run[FUMBLED].boost < 0.15f, "at 1.6 s it has begun to refill");
    launch_all(20.f, 1.0);
    check(near_boost(FUMBLED, 0.1f + 1.1f / 3.f), "at the normal rate");
    launch_all(20.f, 6.0);
    check(g_phase.load() == P_NONE, "the run ends when it is full");
    check(logged("launch done: fumbled AI car 2: 50 km/h after 2.5"), "the log says what the fumble cost");
    check(logged("launch done: fumbled AI car 3: 50 km/h after 2.5"), "for every fumbled car");
    check(!logged("launch done: fumbled AI car 4") && !logged("launch done: fumbled AI car 5"), "and for no other");

    // In the physics of the game log. Each time counts from when the car moves.
    auto ai_to_50_kmh = [](float boost, bool hold, float *peak = nullptr) {
        reset("the AI's fumble in a closed loop");
        session_start("grand_prix", "race");
        enter();
        tick();
        run_to_lights_out(11000);
        constexpr uint32_t CAR = 3;
        for (uint32_t c = 0; c < RACERS; c++) {
            set_ai_start(c, 1.f);
            g_car[c].kmh = c == CAR ? 0.f : 60.f;
        }
        set_ai_start(CAR, boost, hold);
        double t = 0, dt = 1.0 / 60.0;
        for (; g_car[CAR].kmh < 50.f && t < 11.0; t += dt) {
            g_car[CAR].kmh += (AI_LAUNCH * launch_accel(g_run[CAR].power) + MGUK_EFFECT * deploy_of(CAR) / ERS_2026) * float(dt) * 3.6f;
            tick(dt);
        }
        if (peak)
            *peak = g_live[CAR].deploy_below.load();
        return float(t);
    };
    float without = ai_to_50_kmh(0.1f, false), peak = 0, fumbled = ai_to_50_kmh(0.1f, true, &peak), poor = ai_to_50_kmh(0.35f, true), good = ai_to_50_kmh(0.75f, true);
    printf("flow_test: closed loop, AI car from moving to 50 km/h: fumbled %.2f s (%.2f s with no hold, peak %.0f W), poor %.2f s, good %.2f s\n", fumbled, without, peak, poor, good);
    check(without > 2.25f && without < 2.65f, "with no hold, what the game log shows: 2.62 to 2.68 s from lights out, 0.2 s of that the reaction");
    check(good > 1.75f && good < 2.15f, "a good start, as the game log's median");
    check(poor > good + 0.1f && poor < without, "a poor start costs a little");
    check(fumbled > without + 0.3f, "a fumble costs more than it did");
    check(fumbled < 3.6f, "and never more than the net allows");
}

static void test_ai_spread()
{
    reset("the AI's starts vary");
    session_start("grand_prix", "race");
    for (uint32_t i = 0; i < RACERS; i++) {
        g_live[i].seen = true;
        g_live[i].max_deploy = ERS_2026;
        g_live[i].ai = i != PLAYER;
    }
    gs::Tuning t;
    int good = 0, poor = 0, fumbled = 0, total = 0, held = 0, held_right = 0;
    float lowest = 1.f, highest = 0.f;
    for (int n = 0; n < 200; n++) {
        for (auto &c : g_run)
            c = CarRun{};
        begin_launch(t);
        check(g_run[PLAYER].hold_s == 0.f, "the player has no hold: the player's refill follows the revs");
        for (uint32_t i = 0; i < RACERS; i++) {
            if (i == PLAYER)
                continue;
            float b = g_run[i].boost;
            total++;
            held += g_run[i].hold_s > 0.f;
            held_right += g_run[i].hold_s == (b < 0.2f ? t.ai_fumble_hold_s : 0.f);
            fumbled += b < 0.2f;
            poor += b >= 0.2f && b < 0.5f;
            good += b >= 0.5f;
            lowest = std::min(lowest, b);
            highest = std::max(highest, b);
        }
        end_run("test");
    }
    check(fumbled > total * 0.03 && fumbled < total * 0.07, "about 1 in 20 fumbled");
    check(poor > total * 0.16 && poor < total * 0.24, "about 1 in 5 poor");
    check(good > total * 0.71 && good < total * 0.79, "the rest clean");
    check(lowest < 0.02f && highest > 0.98f, "the whole range is used");
    check(held == fumbled && held_right == total, "exactly the fumbled cars are held");
    check(logged("AI spool at lights out: "), "logged");
    wait_layers();
    for (auto &l : g_live)
        l.seen = false;

    reset("the AI's times in the summary");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    // Each car at its own pace, the first in the table the slowest.
    for (int i = 0; i < 60 * 6 && g_phase.load() != P_NONE; i++) {
        for (uint32_t c = 0; c < RACERS; c++)
            g_car[c].kmh += (40.f + 5.f * float(c)) / 60.f;
        tick();
    }
    check(g_phase.load() == P_NONE, "the run ended");
    std::string line = log_line("launch done: AI cars at 50 km/h: 21 of 21");
    float first = 0, median = 0, last = 0;
    size_t at = line.find("first ");
    check(at != std::string::npos && sscanf_s(line.c_str() + at, "first %f s, median %f s, the last after %f s", &first, &median, &last) == 3, "the summary reads");
    check(first > 0.3f && first < 0.4f, "the first AI car: 145 km/h per second");
    check(median > first && median < last, "the median is between them");
    check(last > 1.2f && last < 1.3f, "the last AI car: 40 km/h per second");
    check(logged("launch done: worst frame "), "the worst frame is in the log, to tell a hitch from the next game log");
}

static void test_launch_window_and_freeze()
{
    reset("launch window");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(4000);
    for (uint32_t c = 0; c < RACERS; c++)
        g_car[c].kmh = 20.f;
    ticks(5.0);
    check(g_phase.load() == P_LAUNCH, "still in the window");
    float since = g_launch_s;
    // Frozen: frames go on, the physics steps have no time.
    for (int i = 0; i < 100; i++) {
        run_steps(1.0 / 60.0, false);
        frame_cb(1.0 / 60.0, nullptr);
    }
    check(g_launch_s == since, "the launch clock stops with the game");
    Sleep(int(FROZEN_MS) + 60);
    frame_cb(1.0 / 60.0, nullptr);
    check(g_frozen.load(), "the overlay knows");
    ticks(7.2);
    check(g_phase.load() == P_NONE, "the window closes after 12 s of played time");
    wait_layers();
    check(layer_count() == 0, "layers gone");
    check(logged("launch window over"), "logged");
}

static void test_state_gone()
{
    reset("the state object changes");
    session_start("grand_prix", "race");
    enter();
    tick();
    g_st->at<uintptr_t>(0) = 0xdead;
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_NONE, "the run ends");
    check(logged("the lights state is gone"), "logged");

    reset("the state object is freed");
    session_start("grand_prix", "race");
    auto *page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto at = [&](uintptr_t o) -> uint8_t * { return page + o; };
    *reinterpret_cast<uintptr_t *>(at(0)) = 0x1457e25b0;
    *reinterpret_cast<uintptr_t *>(at(ST_LIGHTS)) = g_sl->p();
    *reinterpret_cast<uintptr_t *>(at(ST_SESSION)) = g_session->p();
    enter_hook(page, nullptr, nullptr, nullptr);
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_PRE, "running on that object");
    VirtualFree(page, 0, MEM_RELEASE);
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_NONE, "ended without a fault");

    // Half of it gone: the vtable still reads, what follows does not.
    reset("half the state object is freed");
    session_start("grand_prix", "race");
    page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    uint8_t *obj = page + 0x1000 - 0x100;
    *reinterpret_cast<uintptr_t *>(obj) = 0x1457e25b0;
    *reinterpret_cast<uintptr_t *>(obj + ST_LIGHTS) = g_sl->p();
    *reinterpret_cast<uintptr_t *>(obj + ST_SESSION) = g_session->p();
    enter_hook(obj, nullptr, nullptr, nullptr);
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_PRE, "running on that object");
    long faults = g_faults.load();
    VirtualFree(page + 0x1000, 0x1000, MEM_DECOMMIT);
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_NONE, "the run ends");
    check(g_faults.load() == faults, "no fault for the core's crash handler to report");
    VirtualFree(page, 0, MEM_RELEASE);
}

// The offsets the code uses are the ones in the instructions the build check compares with the game.
static const CodeCheck &checked(const char *what)
{
    for (const auto &c : CODE_CHECKS)
        if (!strcmp(c.what, what))
            return c;
    printf("FAIL [offsets] no build check named %s\n", what);
    g_fails++;
    return CODE_CHECKS[0];
}
static uintptr_t disp32(const CodeCheck &c, size_t at)
{
    uint32_t v = 0;
    memcpy(&v, c.bytes + at, 4);
    return v;
}

static void test_offsets()
{
    g_test = "offsets";
    check(disp32(checked("the countdown"), 4) == ST_DELAY, "the countdown is the delay");
    const CodeCheck &lights = checked("lights object, state and mode");
    check(disp32(lights, 3) == ST_LIGHTS, "lights object");
    check(lights.bytes[9] == SL_STATE && lights.bytes[10] == 0, "lights state, tested against idle");
    check(lights.bytes[15] == SL_MODE && lights.bytes[16] == SL_MODE_FORMATION, "lights mode, tested against the formation's");
    const CodeCheck &done = checked("lights finished");
    check(done.bytes[2] == SL_STATE && done.bytes[3] == 4, "the state the update routes on");
    check(disp32(checked("lights reset, enabled flag"), 13) == SL_ENABLED, "enabled flag");
    const CodeCheck &sess = checked("session pointer, pause state");
    check(disp32(sess, 3) == ST_SESSION && disp32(sess, 9) == SESSION_STATE && sess.bytes[13] == SESSION_PAUSED, "session, its state, paused");
    check(disp32(checked("first-run flag"), 2) == ST_ONCE, "first-run flag");
    const CodeCheck &pt = checked("powertrain car and engine");
    check(disp32(pt, 6) == DYN_ENGINE && pt.bytes[13] == DYN_CAR, "engine and car of the powertrain's argument");
    check(disp32(checked("engine speed"), 4) == ENGINE_RAD_S, "engine speed");
    check(disp32(checked("car ERS"), 3) == CAR_ERS, "car ERS");
    check(checked("max deploy, target").bytes[5] == ERS_MAX_DEPLOY, "max deploy, read for the target");
    check(checked("max deploy, cap").bytes[5] == ERS_MAX_DEPLOY, "max deploy, read for the cap");
    check(disp32(checked("deploy written"), 5) == ERS_DEPLOY, "deploy written");
    check(disp32(checked("substep time"), 4) == DYN_DT, "substep time");
    check(disp32(checked("body position"), 3) == BODY_POS, "body position");
    check(disp32(checked("body velocity"), 3) == BODY_VEL, "body velocity");
    const CodeCheck &ch = checked("car chassis, its rigid body");
    check(disp32(ch, 3) == CAR_CHASSIS && disp32(ch, 10) == CH_RIGID, "chassis and rigid");
    check(checked("the body").bytes[3] == RIGID_BODY, "the body");
    check(disp32(checked("AI flag"), 2) == CAR_IS_AI && checked("AI flag").bytes[6] == 1, "AI flag, tested against 1");
    check(disp32(checked("session descriptor"), 3) == SESSION_DESC, "session descriptor");
    check(disp32(checked("vehicle id"), 4) == CAR_VEHICLE_ID, "vehicle id");
    check(checked("system owner").bytes[3] == SYS_OWNER, "system owner");
    const CodeCheck &hv = checked("handling version, against 2026's");
    check(disp32(hv, 2) == CAR_HANDLING && hv.bytes[6] == gs::F1_2026_HANDLING, "the game tests the handling version against the same 9");
    check(disp32(checked("curve last index"), 2) == SPLINE_LAST, "last index");
    const CodeCheck &so = checked("curve scale and offset");
    check(so.bytes[2] == SPLINE_SCALE && so.bytes[6] == SPLINE_OFFSET && so.bytes[9] == SPLINE_SCALE + OBF_KEY, "scale, offset, the scale's key");
    check(checked("curve offset key").bytes[3] == SPLINE_OFFSET + OBF_KEY, "the offset's key");
    const CodeCheck &fs = checked("curve first sample");
    check(disp32(fs, 2) == SPLINE_SAMPLES && disp32(fs, 8) == SPLINE_SAMPLES + OBF_KEY, "first sample and its key");
    const CodeCheck &ls = checked("curve last sample");
    check(disp32(ls, 3) == SPLINE_SAMPLES && disp32(ls, 10) == SPLINE_SAMPLES + OBF_KEY, "last sample and its key: with a last index of 0, the first");
    const CodeCheck &lo = checked("the loader's scale and offset of 0");
    check(lo.bytes[3] == SPLINE_SCALE + OBF_KEY && lo.bytes[7] == SPLINE_SCALE, "the loader writes the scale's key over it: 0");
    check(lo.bytes[11] == SPLINE_OFFSET + OBF_KEY && lo.bytes[15] == SPLINE_OFFSET, "and the offset's");
    const CodeCheck &dc = checked("deploy curve, first and second");
    uintptr_t add = checked("deploy curve call").bytes[3];
    check(dc.bytes[3] + add == ERS_CURVE && disp32(dc, 7) + add == ERS_CURVE_2, "the two curves, with what the call adds to them");
    check(disp32(checked("deploy curve flag"), 3) == ERS_CURVE_FLAG, "the flag that picks the curve");
    check(disp32(checked("zone update shader switch"), 2) == MARSHAL_SHADERS, "the system's shader switch, tested against 0");
    check(checked("zone model").bytes[3] == ZONE_MODEL, "the zone's model");
    check(disp32(checked("zone model instance"), 3) == MODEL_INSTANCE, "the model's instance");
    check(checked("zone colour sent").bytes[3] == ZONE_COLOUR, "what the update sends starts at the colour");
    check(checked("two values sent").bytes[2] == 2 && ZONE_MODE == ZONE_COLOUR + 0x10, "two float4s: the colour and the mode behind it");
    check(checked("panels off, colour").bytes[5] == ZONE_COLOUR && checked("panels off, mode").bytes[5] == ZONE_MODE, "the game's off write");
    check(checked("blue flag, colour").bytes[5] == ZONE_COLOUR && checked("blue flag, mode").bytes[5] == ZONE_MODE, "the game's blue flag write");
}

static void test_timeout()
{
    reset("no lights out");
    session_start("grand_prix", "race");
    enter();
    tick();
    g_hold_s = 1000.f;
    ticks(46.0, 0.1);
    check(g_phase.load() == P_NONE, "the run gives up after 40 s");
    check(logged("no lights out"), "logged");
    wait_layers();
    check(layer_count() == 0, "no layers");
}

static void test_no_engine_data()
{
    reset("the powertrain hook never runs");
    session_start("grand_prix", "race");
    enter();
    frame_cb(1.0 / 60.0, nullptr);
    check(g_phase.load() == P_PRE, "the run starts without engine data");
    for (int i = 0; i < 60 * 12 && g_phase.load() != P_NONE; i++) {
        fake_update(1.0 / 60.0);
        frame_cb(1.0 / 60.0, nullptr);
    }
    check(g_phase.load() == P_NONE && logged("no engine data"), "ends at lights out with no layers");
    check(layer_count() == 0, "nothing applied blind");
}

static void test_no_car_ops()
{
    reset("no power field");
    session_start("grand_prix", "race");
    g_power_field = 0;
    enter();
    tick();
    run_steps(0.1, true);
    for (int i = 0; i < 60 * 12 && !g_routed; i++)
        tick();
    tick();
    check(g_phase.load() == P_NONE && logged("no engine.power_scale"), "ends at lights out");
    g_power_field = 32;
}

static void test_threads()
{
    // The real thread layout: steps on their own threads while frames and layer changes run.
    reset("steps on other threads");
    session_start("grand_prix", "race");
    enter();
    tick();
    run_to_lights_out(11000);
    g_run[3].flagged = true; // its curve changes on its own thread, every step
    std::atomic<bool> stop{false};
    std::vector<HANDLE> threads;
    struct Arg {
        Car *c;
        std::atomic<bool> *stop;
    };
    static Arg args[RACERS];
    for (uint32_t i = 0; i < RACERS; i++) {
        args[i] = {&g_car[i], &stop};
        g_car[i].dyn.at<float>(DYN_DT) = 1.f / 360.f;
        threads.push_back(CreateThread(nullptr, 0, [](void *p) -> DWORD {
            auto *a = static_cast<Arg *>(p);
            while (!a->stop->load())
                powertrain_hook(reinterpret_cast<void *>(a->c->dyn.p()));
            return 0;
        }, &args[i], 0, nullptr));
    }
    for (int i = 0; i < 400 && g_phase.load() != P_NONE; i++) {
        for (uint32_t c = 0; c < RACERS; c++)
            g_car[c].kmh = float(i) * 0.5f;
        for (uint32_t c = 0; c < RACERS; c++)
            g_car[c].body.at<float>(BODY_VEL + 8) = g_car[c].kmh / 3.6f;
        frame_cb(1.0 / 60.0, nullptr);
        Sleep(1);
    }
    stop = true;
    WaitForMultipleObjects(DWORD(threads.size()), threads.data(), TRUE, 3000);
    for (HANDLE h : threads)
        CloseHandle(h);
    wait_layers();
    bool restored = true;
    for (uint32_t c = 0; c < RACERS; c++)
        restored = restored && g_car[c].ers.at<float>(ERS_MAX_DEPLOY) == ERS_2026;
    check(restored, "no car's max deploy changed");
    check(g_live[3].deploy_below.load() > 0.f, "the flagged car deployed on its thread");
    bool intact = true;
    for (uint32_t c = 0; c < RACERS; c++)
        intact = intact && curves_intact(g_car[c]);
    check(intact, "and every car's curves are the game's");
    check(g_phase.load() == P_NONE, "the run ended");
    check(layer_count() == 0, "every layer is gone");
}

// ------------------------------------------------------------------------------------------------
// The HUD: the game's own lap counter and message box, the overlay's lights and gauge

static void test_game_hud()
{
    reset("the game's lap counter in Pre-Start");
    session_start("grand_prix", "race");
    enter();
    tick();
    check(lap_says_pre_start(), "PRE-START as literal text, the lap number and total hidden");
    check(logged("lap counter shows PRE-START"), "logged");
    int writes = g_leaf_writes;
    check(writes == 4, "four leaves");
    tick();
    check(g_leaf_writes == writes, "not again at once");
    g_leaves.clear(); // the game wrote its own values
    Sleep(int(LAP_ROW_EVERY_S * 1000.0) + 30);
    tick();
    check(lap_says_pre_start() && g_leaf_writes == writes + 4, "written again after half a second");
    g_car[PLAYER].rpm = 11000;
    ticks(5.0);
    check(g_phase.load() == P_LIGHTS, "the lights run");
    check(lap_number_back() && log_count("lap counter back to the lap number") == 1, "the lap number is back with the first red light");
    writes = g_leaf_writes;
    run_to_lights_out(11000);
    launch_all(40.f, 4.0);
    check(g_leaf_writes == writes, "nothing more in the run");
    check(g_toasts.empty(), "good starts: no message");

    reset("the game's lap counter: the session ends in Pre-Start");
    session_start("grand_prix", "race");
    enter();
    tick();
    check(lap_says_pre_start(), "showing");
    fire(SWOFTY_EVENT_SESSION_END);
    tick();
    check(g_phase.load() == P_NONE && lap_number_back(), "the lap number is back");

    reset("SLOW CAR AHEAD waits for the player's own check");
    net_start(3, 11000);
    g_car[3].z = 50.f;
    g_car[PLAYER].z = 0.f;
    g_car[PLAYER].kmh = 0.f; // the player moves off half a second after the others
    ticks(0.5);
    g_car[PLAYER].kmh = 40.f;
    ticks(0.75);
    check(g_run[3].flagged && g_ahead_flag.load(), "car 3 is flagged and ahead");
    check(!g_run[PLAYER].checked && toasts_of(MSG_AHEAD) == 0, "no message before the player's own check");
    ticks(0.5);
    check(g_run[PLAYER].checked && !g_run[PLAYER].flagged, "the player's own start is fine");
    check(toasts_of(MSG_AHEAD) == 1 && toasts_of(MSG_LOW) == 0, "then SLOW CAR AHEAD");
    ticks(2.0);
    check(toasts_of(MSG_AHEAD) == 1, "once");

    reset("no SLOW CAR AHEAD after the player's own message");
    net_start(PLAYER); // the player's boost is 0
    g_car[PLAYER].kmh = 10.f; // moving, gaining nothing
    g_car[4].kmh = 0.f;       // car 4, 8 m ahead, moves off a second later
    ticks(1.0);
    g_car[4].kmh = 5.f;
    g_run[4].boost = 0.f;
    ticks(1.2);
    check(g_run[PLAYER].flagged && g_run[4].flagged, "both flagged, a second apart");
    ticks(4.5);
    check(!g_player_flag.load() && g_ahead_flag.load(), "the player's 5 s are over, car 4's are not");
    check(toasts_of(MSG_LOW) == 1 && toasts_of(MSG_AHEAD) == 0, "one message: the player's own");
}

static void test_hud_fallback()
{
    reset("no game HUD: PRE-START on the overlay");
    g_hud_up = 0;
    session_start("grand_prix", "race");
    enter();
    tick();
    tick();
    check(g_leaf_writes == 0, "nothing written to the game's HUD");
    check(lights_on_screen(), "the blue lights");
    bool steady = true;
    for (int i = 0; i < 6; i++) {
        steady = steady && painted("PRE-START");
        Sleep(90);
    }
    check(steady, "PRE-START in a stand-in for the game's message box, steady");

    reset("no game HUD: LOW-OUTPUT START on the overlay");
    g_hud_up = 0;
    net_start(PLAYER);
    ticks(1.2);
    check(g_run[PLAYER].flagged && g_toasts.empty(), "flagged, no game message");
    check(logged("game message LOW-OUTPUT START: no game HUD, the overlay shows it"), "logged");
    steady = true;
    for (int i = 0; i < 6; i++) {
        steady = steady && painted(MSG_LOW) && drawn_text(MSG_LOW_SUB);
        Sleep(90);
    }
    check(steady, "on the overlay with its second line, steady");
    paint();
    check(drawn_count('p', AMBER) == 1 && drawn_count('l', INK) == 1, "with the warning triangle");
    ticks(5.0);
    check(!painted(MSG_LOW), "gone after 5 s");

    reset("the game HUD refuses");
    g_hud_set_result = g_hud_toast_result = SWOFTY_ASSET_UNAVAILABLE;
    session_start("grand_prix", "race");
    enter();
    tick();
    check(logged("lap counter: the game HUD refused a write (result 2)"), "logged");
    check(painted("PRE-START"), "the overlay shows PRE-START instead");
    Sleep(int(LAP_ROW_EVERY_S * 1000.0) + 30);
    ticks(1.0);
    check(log_count("refused a write") == 1, "logged once, and not tried again in this run");

    reset("the next start tries the game HUD again");
    session_start("grand_prix", "race");
    enter();
    tick();
    check(lap_says_pre_start() && !painted("PRE-START"), "the lap counter again, no stand-in");

    reset("the game's message box refuses SLOW CAR AHEAD");
    g_hud_toast_result = SWOFTY_ASSET_INVALID;
    net_start(3, 11000);
    g_car[3].z = 50.f;
    g_car[PLAYER].z = 0.f;
    ticks(1.2);
    check(logged("game message SLOW CAR AHEAD refused (result 3), the overlay shows it"), "logged");
    check(painted(MSG_AHEAD) && !drawn_text(MSG_LOW), "on the overlay");
    g_car[3].z = -50.f; // passed
    tick();
    check(!painted(MSG_AHEAD), "gone once the car is behind");
}

static int segments(uint32_t rgba)
{
    float k = std::min(g_screen_w / 1920.f, g_screen_h / 1080.f);
    return int(std::count_if(g_drawn.begin(), g_drawn.end(), [&](const Drawn &d) { return d.kind == 'R' && d.rgba == rgba && std::fabs(d.h - 13.f * k) < 0.01f; }));
}

static const Drawn *gauge_body()
{
    for (const Drawn &d : g_drawn)
        if (d.kind == 'R' && d.rgba == 0x0C0F18ED)
            return &d;
    return nullptr;
}

static bool lights_at(float cx, float y, float k)
{
    std::vector<Light> v = lights_drawn();
    bool at = v.size() == 5;
    for (size_t i = 0; at && i < 5; i++)
        at = std::fabs(v[i].x - (cx + (float(i) - 2.f) * 82.f * k)) < 0.01f && std::fabs(v[i].y - y) < 0.01f && std::fabs(v[i].r - 32.f * k) < 0.01f;
    return at;
}

static void test_hud_layout()
{
    reset("HUD layout at 1920 x 1080");
    session_start("grand_prix", "race");
    enter();
    tick();
    tick();
    check(lights_at(960.f, 338.f, 1.f), "on the game's red lights: x 796 to 1124, 82 apart, y 338, 64 across");
    std::vector<Light> v = lights_drawn();
    check(std::all_of(v.begin(), v.end(), [&](const Light &l) { return l.lit == v[0].lit; }), "all five together");
    bool lit = false, dark = false;
    for (int i = 0; i < 70 && !(lit && dark); i++) {
        for (const Light &l : lights_drawn())
            (l.lit ? lit : dark) = true;
        Sleep(10);
    }
    check(lit && dark, "flashing");
    paint();
    int dots = drawn_count('c', 0xBEDAFFCC) + drawn_count('c', 0xBEDAFF29);
    check(dots % 5 == 0 && dots >= 5 * 50 && dots <= 5 * 90, "a face of LED dots on each");

    g_player_boost = 0.43f;
    paint();
    const Drawn *body = gauge_body();
    check(body && body->x == 810.f && body->y == 788.f && body->w == 300.f && body->h == 63.f, "the gauge above the dash: 300 x 63 at (810, 788)");
    check(segments(LIME) == 6 && segments(0xFFFFFF21) == 8, "43 %: 6 of 14 segments lit");
    const Drawn *value = drawn_text("43%"), *head = drawn_text("TURBO SPOOL");
    check(value && value->align == (SWOFTY_TEXT_RIGHT | SWOFTY_TEXT_MIDDLE) && value->x == 1097.f, "the value, right-aligned 13 px in");
    check(head && head->align == (SWOFTY_TEXT_CENTER | SWOFTY_TEXT_MIDDLE) && head->x == 960.f && head->y == 797.5f, "the header, centred in its cell");
    g_player_boost = 0.999f;
    paint();
    check(segments(LIME) == 13 && drawn_text("99%"), "not full yet: 13 segments and 99 %");
    g_player_boost = 1.f;
    paint();
    check(segments(GREEN) == 14 && drawn_text("FULL") && segments(LIME) == 0, "full: 14 green segments and FULL");
    check(drawn_count('g', alpha(GREEN, 0x61)) == 1, "the bottom glow in the gauge's colour");

    reset("HUD layout at 3440 x 1440");
    g_screen_w = 3440;
    g_screen_h = 1440;
    session_start("grand_prix", "race");
    enter();
    tick();
    tick();
    float k = 1440.f / 1080.f;
    check(lights_at(1720.f, 338.f * k, k), "scaled to the height, centred");
    paint();
    body = gauge_body();
    check(body && std::fabs(body->x - 1520.f) < 0.01f && std::fabs(body->y - 788.f * k) < 0.01f && std::fabs(body->w - 400.f) < 0.01f, "the gauge too");

    reset("HUD layout at 1920 x 1200");
    g_screen_w = 1920;
    g_screen_h = 1200;
    session_start("grand_prix", "race");
    enter();
    tick();
    tick();
    check(lights_at(960.f, 398.f, 1.f), "the 16:9 layout, centred: 60 px lower");
    paint();
    body = gauge_body();
    check(body && body->y == 848.f, "the gauge too");
}

static void test_overlay_v1()
{
    reset("an overlay without v2");
    g_ov = &g_fake_overlay_v1;
    g_ov_ver = 1;
    g_hud_up = 0;
    session_start("grand_prix", "race");
    enter();
    tick();
    tick();
    check(lights_at(960.f, 338.f, 1.f), "the lights, as squares");
    check(painted("TURBO SPOOL") && painted("PRE-START"), "the gauge and the words");
    check(std::all_of(g_drawn.begin(), g_drawn.end(), [](const Drawn &d) { return strchr("rolt", d.kind) != nullptr; }), "only what a v1 overlay has");

    reset("an overlay without v2: a warning");
    g_ov = &g_fake_overlay_v1;
    g_ov_ver = 1;
    g_hud_up = 0;
    net_start(PLAYER);
    ticks(1.2);
    check(painted(MSG_LOW), "LOW-OUTPUT START");
}

static SwoftyApi make_api()
{
    SwoftyApi a{};
    a.version = SWOFTY_API_VERSION;
    a.log = fake_log;
    a.module_base = api_base;
    a.register_mixins = api_mixins;
    a.setting = api_setting;
    a.watch_setting = api_watch;
    a.events = &g_fake_events;
    a.overlay = &g_fake_overlay;
    a.hooks = &g_fake_hooks;
    return a;
}

static void test_init()
{
    g_test = "init";
    static SwoftyApi api = make_api();
    size_t size = 0;
    for (const auto &c : CODE_CHECKS)
        size = std::max(size, size_t(c.rva + c.n));
    auto *image = static_cast<uint8_t *>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    g_image = reinterpret_cast<uintptr_t>(image);
    for (const auto &c : CODE_CHECKS)
        memcpy(image + c.rva, c.bytes, c.n);

    // Another build: nothing is registered.
    uint8_t keep = image[CODE_CHECKS[7].rva];
    image[CODE_CHECKS[7].rva] ^= 0xff;
    check(SwoftyAddonInit(&api) == 1, "a failed check is not a failed load");
    check(g_mixins.empty(), "another build: no hooks");
    check(logged("idle: this game build differs"), "another build: logged");
    check(!g_frame_fn, "another build: no frame callback");
    image[CODE_CHECKS[7].rva] = keep;

    // The older API
    SwoftyApi old = api;
    old.version = 8;
    SwoftyAddonInit(&old);
    check(g_mixins.empty() && logged("needs Swofty API v9"), "older API: idle");

    g_fake_enter_fn = reinterpret_cast<void *>(&fake_enter);
    g_fake_step_fn = reinterpret_cast<void *>(&fake_step);
    g_fake_zone_fn = reinterpret_cast<void *>(&fake_zone_update);
    g_settings = {{"pre_start", 1}, {"launch_model", 1}, {"safety_net", 1}, {"apply_to_ai", 1}, {"spool_seconds", 3}, {"spool_rpm", 10000}};
    check(SwoftyAddonInit(&api) == 1, "loads");
    check(g_mixins.size() == 3, "three mixins");
    bool enter_wrap = false, step_wrap = false, zone_wrap = false;
    for (const auto &m : g_mixins) {
        enter_wrap = enter_wrap || (!strcmp(m.target, "Sess_StartLights_Enter") && m.kind == SWOFTY_MIXIN_WRAP && m.orig && m.handler);
        step_wrap = step_wrap || (!strcmp(m.target, "Car_Powertrain_Step") && m.kind == SWOFTY_MIXIN_WRAP && m.orig && m.handler);
        zone_wrap = zone_wrap || (!strcmp(m.target, "Sess_MarshalLights_UpdateZoneShader") && m.kind == SWOFTY_MIXIN_WRAP && m.orig && m.handler);
        check(SWOFTY_MIXIN_KIND(m.kind) != SWOFTY_MIXIN_SITE, "no site: the core owns the torque site, and a site there drops the whole addon");
    }
    check(enter_wrap && step_wrap && zone_wrap, "wraps on the lights enter, the powertrain and the zone update");
    check(g_enter_orig && g_powertrain_orig && g_zone_orig, "originals kept");
    check(g_frame_fn == frame_cb && g_session_fn == on_session_hook, "frame and session.start attached");
    check(std::find(g_events.begin(), g_events.end(), SWOFTY_EVENT_PAUSE) == g_events.end(), "no pause event: the catalogue says it never fires");
    check(std::find(g_events.begin(), g_events.end(), SWOFTY_EVENT_RACE_START) == g_events.end(), "no race.start: it fires before the lights");
    check(logged(" ready: build check ok"), "ready logged");
    check(logged(", game HUD no"), "no game HUD without the core's export");
    check(g_want[0].load() == 1.f && g_want[MAX_CARS - 1].load() == 1.f, "no power wanted at the start");

    // The core tables come from swofty_core.dll, which this test does not have.
    g_cars = &g_fake_cars;
    g_power_field = 32;
    start_layer_worker();
    check(g_layer_event != nullptr, "layer thread");

    frame_cb(1.0 / 60.0, nullptr);
    check(g_paint_fn == paint_cb, "the overlay subscribes from a frame");
    VirtualFree(image, 0, MEM_RELEASE);
}

static void test_real_exe()
{
    g_test = "real exe";
    const wchar_t *path = L"F:\\SteamLibrary\\steamapps\\common\\F1 25\\F1_25.exe";
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        puts("flow_test: the game's exe is not here, build bytes not compared");
        return;
    }
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READONLY, 0, 0, nullptr);
    auto *d = m ? static_cast<const uint8_t *>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0)) : nullptr;
    check(d != nullptr, "mapped");
    if (d) {
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(d);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(d + dos->e_lfanew);
        auto *sec = IMAGE_FIRST_SECTION(nt);
        int ok = 0, total = 0;
        for (const auto &c : CODE_CHECKS) {
            total++;
            for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
                if (c.rva >= sec[i].VirtualAddress && c.rva < sec[i].VirtualAddress + sec[i].Misc.VirtualSize) {
                    bool same = memcmp(d + sec[i].PointerToRawData + (c.rva - sec[i].VirtualAddress), c.bytes, c.n) == 0;
                    ok += same;
                    if (!same)
                        printf("FAIL [real exe] %s at 0x%llx\n", c.what, static_cast<unsigned long long>(c.rva));
                    break;
                }
        }
        check(ok == total, "every build check matches the installed game");
        auto game_value = [&](uintptr_t rva) -> const uint8_t * {
            for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
                if (rva >= sec[i].VirtualAddress && rva + 16 <= sec[i].VirtualAddress + sec[i].Misc.VirtualSize)
                    return d + sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
            return nullptr;
        };
        const uint8_t *blue = game_value(0x53829d0), *lit = game_value(0x55d85b0);
        check(blue && !memcmp(blue, PANEL_BLUE, 16), "the panel's blue is the value the game's blue flag is made from");
        check(lit && !memcmp(lit, PANEL_LIT, 16), "the panel's mode is the game's plain lit panel");
        const float none[4] = {};
        check(!memcmp(PANEL_OFF, none, 16), "off is all zero, as the game's update writes it");
        printf("flow_test: %d of %d build checks match the installed game\n", ok, total);
        UnmapViewOfFile(d);
    }
    if (m)
        CloseHandle(m);
    CloseHandle(f);
}

static void test_manifest()
{
    g_test = "manifest";
    std::ifstream in("..\\swofty.json");
    std::stringstream b;
    b << in.rdbuf();
    std::string s = b.str();
    check(!s.empty(), "swofty.json is readable");
    check(s.find(std::string("\"version\": \"") + GS_VERSION + "\"") != std::string::npos, "the manifest version is the code's");
    for (const char *id : {"pre_start", "launch_model", "safety_net", "apply_to_ai", "spool_seconds", "spool_rpm"})
        check(s.find(std::string("\"id\": \"") + id + "\"") != std::string::npos, id);
    check(s.find("ice_share") == std::string::npos, "the 2025-car slider is gone");
}

int main(int argc, char **)
{
    g_verbose = argc > 1;
    AddVectoredExceptionHandler(1, count_faults);
    build_world();
    test_init();
    test_real_exe();
    test_offsets();
    test_manifest();
    test_normal_start();
    test_trackside_panels();
    test_trackside_panels_frozen();
    test_trackside_panels_not_on();
    test_formation_lap_leftover();
    test_formation_lap_leftover_no_pre_start();
    test_game_delay();
    test_only_f1_2026();
    test_no_revs();
    test_rolling_before_lights_out();
    test_revs_fall_at_launch();
    test_physics_clock();
    test_boost_decays_while_waiting();
    test_pause_in_pre_start();
    test_freeze_without_pause_state();
    test_reenter();
    test_restart_after_race();
    test_restart_during_launch();
    test_session_end_during_launch();
    test_gates();
    test_career_weekend();
    test_settings();
    test_safety_net();
    test_net_closed_loop();
    test_ai_fumble();
    test_ai_spread();
    test_launch_window_and_freeze();
    test_state_gone();
    test_timeout();
    test_no_engine_data();
    test_no_car_ops();
    test_game_hud();
    test_hud_fallback();
    test_hud_layout();
    test_overlay_v1();
    test_threads();
    g_test = "whole run";
    check(g_faults.load() == 0, "no access violation in the whole run: the core's crash handler would report each");
    printf("flow_test %s: %d checks, %d failed\n", g_fails ? "FAILED" : "ok", g_checks, g_fails);
    fflush(stdout);
    ExitProcess(g_fails ? 1 : 0); // the layer thread never returns
}
