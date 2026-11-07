#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../src/model.h"

using namespace gs;

static int g_fail;
static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        g_fail = 1;
    }
}
static bool near_(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main()
{
    Tuning t;
    float b = 0;
    for (int i = 0; i < 30; i++)
        b = spool_step(b, true, 0.1f, t);
    check(near_(b, 1.f), "3 s at revs gives full boost");
    check(near_(spool_step(1.f, false, 1.f, t), 1.f - 1.f / 6.f), "boost decays at half the rise rate while waiting");
    check(near_(launch_boost(0.5f, false, 1.f, t), 0.5f), "after lights out low revs keep the boost");
    check(near_(launch_boost(0.5f, true, 0.3f, t), 0.6f), "after lights out high revs still build boost");
    check(is_f1_2026(9, 350e3f), "the game's F1 2026 data");
    check(!is_f1_2026(8, 126.3e3f), "the game's F1 2025 data");
    check(!is_f1_2026(7, 126.3e3f), "the game's F1 2024 data");
    check(!is_f1_2026(8, 350e3f), "2026 power on older handling data");
    check(!is_f1_2026(9, 126.3e3f), "2026 handling data with older power");
    check(!is_f1_2026(9, 0.f), "2026 handling data with no ERS");
    check(!is_f1_2026(100, 350e3f), "the game's other handling version");
    check(grid_is_f1_2026(22, 0), "a 2026 grid");
    check(grid_is_f1_2026(1, 0), "one 2026 car and no other with an ERS");
    check(!grid_is_f1_2026(0, 0), "no car with an ERS: F2, or nothing read");
    check(!grid_is_f1_2026(0, 20), "an older grid");
    check(!grid_is_f1_2026(21, 1), "a mixed grid");
    check(near_(target_power(1, t), 1.f), "full boost: the engine is untouched");
    check(near_(target_power(0, t), 0.35f), "no boost");
    check(near_(target_power(0.5f, t), 0.675f), "half boost");
    check(near_(target_power(2.f, t), 1.f), "boost is clamped");
    check(!low_output(1.f, 1.f, 0.5f, 0.f, t), "too early to detect");
    check(low_output(3.15f, 3.15f, 1.f, 0.f, t), "a missed spool is low output");
    check(!low_output(5.f, 5.f, 1.f, 0.f, t), "above the line: a normal launch");
    check(!low_output(0.f, 0.f, 1.f, 0.f, t), "a stationary car is left alone");
    check(low_output(5.f, 3.f, 1.f, 0.f, t), "what it gained counts, not the speed it rolled in with");
    check(!low_output(3.f, 3.f, 1.f, 0.5f, t), "half the boost: the trouble is grip or the driver, not power");
    check(low_output(3.f, 3.f, 1.f, 0.49f, t), "less than half the boost");
    check(near_(net_line(3.f, 3.f, 0.1f, t), 3.45f), "the line rises at the minimum acceleration");
    check(near_(net_line(3.f, 1.f, 0.1f, t), 2.f), "never more than 1 m/s ahead of the car");
    check(near_(mguk_share(3.f, 3.f, t), 0.f), "on the line: nothing");
    check(near_(mguk_share(3.f, 5.f, t), 0.f), "above the line: nothing, and never harvest");
    check(near_(mguk_share(3.f, 2.f, t), 0.5f), "below the line: in proportion");
    check(near_(mguk_share(30.f, 2.f, t), 1.f), "at most the whole curve");
    check(near_(ai_boost(0.f, 0.f), 0.f) && near_(ai_boost(0.04f, 1.f), 0.2f), "fumbled: up to 20 %");
    check(near_(ai_boost(0.05f, 0.f), 0.2f) && near_(ai_boost(0.24f, 1.f), 0.5f), "poor: 20 to 50 %");
    check(near_(ai_boost(0.25f, 0.f), 0.5f) && near_(ai_boost(1.f, 1.f), 1.f), "good: 50 to 100 %");
    check(near_(ai_refill_after(0.f, t), 1.5f) && near_(ai_refill_after(0.19f, t), 1.5f), "a fumbled AI car bogs first");
    check(near_(ai_refill_after(0.2f, t), 0.f) && near_(ai_refill_after(1.f, t), 0.f), "a poor or a good one refills at once");
    check(near_(rate_limit(1.f, 0.f, 0.1f, t), 0.7f), "rate limit falls");
    check(near_(rate_limit(0.5f, 0.6f, 1.f, t), 0.6f), "rate limit reaches the target");
    puts(g_fail ? "model_test FAILED" : "model_test ok");
    return g_fail;
}
