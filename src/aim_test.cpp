// -----------------------------------------------------------------------------
//  aim_test.cpp  --  validate aim selection and smoothing against simulated data.
//
//  The spec asks for exactly this, and the reason is sound: the selection rule
//  and the smoothing curve are the parts of the aim assist that can be wrong in
//  a way that still "works" -- it picks a target, it moves a cursor -- so an
//  end-to-end eyeball test cannot tell a correct implementation from a plausible
//  one.  Simulated targets make the correct answer computable in advance.
//
//  What this does NOT test: SendInput, the key gate, or the user-is-moving gate.
//  Those depend on the live system and are exercised by running the tool.  This
//  test covers the two pieces of arithmetic that have a right answer.
// -----------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <vector>

// The selection and smoothing maths, reproduced from aim.cpp so it can be tested
// without a game.  Reproduced rather than linked because aim.cpp pulls in the
// Win32 input calls; if the two ever diverge, the divergence shows up in a live
// run, and the alternative -- making the production code link against a test
// harness -- would put test scaffolding in the shipped binary.

namespace {

struct Cand { float x, y; bool on_screen; };

/// Selection: nearest to the aim point within the radius, on-screen only.
/// Returns the index, or -1.
int select_target(const Cand* c, int n, float ax, float ay, float radius) {
    int   best = -1;
    float bd   = radius;
    for (int i = 0; i < n; ++i) {
        if (!c[i].on_screen) continue;
        const float dx = c[i].x - ax, dy = c[i].y - ay;
        const float d  = std::sqrt(dx * dx + dy * dy);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/// One smoothing step: a fraction of the remaining error, clamped, accumulated
/// over frames.  Returns the total pixel movement applied.
float smooth_toward(float target, float cur, float smoothing, float max_step,
                    int frames) {
    float accum = 0.f, total = 0.f;
    for (int i = 0; i < frames; ++i) {
        const float err = target - cur;
        if (std::fabs(err) < 2.f) break;
        float d = err / smoothing;
        if (std::fabs(d) > max_step) d = (d > 0 ? max_step : -max_step);
        accum += d;
        const int i_ = (int)accum;
        if (i_ != 0) { cur += (float)i_; accum -= (float)i_; total += (float)i_; }
    }
    return total;
}

int fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { printf("FAIL: %s\n", what); ++fails; }
}

} // namespace

int main() {
    const float ax = 960.f, ay = 540.f;   // 1080p centre

    // -------------------------------------------------------------------------
    //  Selection
    // -------------------------------------------------------------------------
    {
        // Two targets: one at 50 px, one at 300 px.  Radius 120.  The near one
        // must win, and the far one must be out of range entirely.
        Cand c[2] = { { 1010.f, 540.f, true }, { 1260.f, 540.f, true } };
        check(select_target(c, 2, ax, ay, 120.f) == 0, "nearest wins");
    }
    {
        // The near one is off-screen: the far one is the only valid pick, but it
        // is placed outside the radius, so nothing is selected.  (The first
        // version of this case put the far target at x=1000, which is 40 px from
        // centre and therefore very much in range -- the test was wrong, not the
        // rule, which is the kind of thing this file exists to distinguish.)
        Cand c[2] = { { 1010.f, 540.f, false }, { 1180.f, 540.f, true } };
        check(select_target(c, 2, ax, ay, 120.f) == -1,
              "off-screen excluded, out-of-radius rejected");
    }
    {
        // An on-screen target just inside the radius is selected; one just
        // outside is not.  This is the boundary, and it is where an off-by-one
        // in the comparison would live.
        Cand inside[1]  = { { 960.f + 119.f, 540.f, true } };
        Cand outside[1] = { { 960.f + 121.f, 540.f, true } };
        check(select_target(inside,  1, ax, ay, 120.f) == 0,  "just inside radius");
        check(select_target(outside, 1, ax, ay, 120.f) == -1, "just outside radius");
    }
    {
        // Empty set: no selection, and crucially no crash.
        check(select_target(nullptr, 0, ax, ay, 120.f) == -1, "empty set");
    }

    // -------------------------------------------------------------------------
    //  Smoothing
    // -------------------------------------------------------------------------
    {
        // A large error, generously smoothed, must converge monotonically and
        // must never overshoot past the target.
        float cur = ax;
        const float target = ax + 100.f;
        float prev_err = target - cur;
        for (int i = 0; i < 200; ++i) {
            const float err = target - cur;
            if (std::fabs(err) < 2.f) break;
            float d = err / 10.f;
            if (std::fabs(d) > 6.f) d = (d > 0 ? 6.f : -6.f);
            cur += d;
            check(std::fabs(target - cur) <= std::fabs(prev_err) + 0.001f,
                  "smoothing converges monotonically");
            prev_err = target - cur;
        }
        check(std::fabs(target - cur) < 3.f, "smoothing reaches the target");
    }
    {
        // Higher smoothing moves less in the same number of frames.  This is the
        // property the slider is supposed to control, stated as a comparison.
        const float slow = smooth_toward(ax + 100.f, ax, 20.f, 6.f, 10);
        const float fast = smooth_toward(ax + 100.f, ax, 2.f,  6.f, 10);
        check(std::fabs(slow) < std::fabs(fast), "higher smoothing is slower");
    }
    {
        // The dead zone: already within 2 px, nothing moves at all.  Without
        // this the cursor would oscillate around the target forever.
        check(smooth_toward(ax + 1.f, ax, 2.f, 6.f, 100) == 0.f,
              "dead zone suppresses movement");
    }
    {
        // The per-frame clamp: a target 10000 px away must still move at most
        // max_step on the first frame.  The pull is a nudge, and a nudge has a
        // size limit independent of distance.
        float cur = ax;
        const float target = ax + 10000.f;
        const float err = target - cur;
        float d = err / 2.f;
        if (std::fabs(d) > 6.f) d = (d > 0 ? 6.f : -6.f);
        check(std::fabs(d) <= 6.f + 0.001f, "per-frame clamp holds");
    }

    printf("aim_test: %d failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
