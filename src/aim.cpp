// -----------------------------------------------------------------------------
//  aim.cpp  --  proportional aim assist.  See aim.hpp for the design rationale.
//
//  The whole of the behaviour is in aim_step.  Everything else is state.
// -----------------------------------------------------------------------------
#include "aim.hpp"

#include "menu.hpp"

#include <algorithm>
#include <cmath>

namespace vex {
namespace bs {

namespace {

// -----------------------------------------------------------------------------
//  Per-session state.
//
//  File-scope rather than a member, because there is exactly one assist in the
//  process and threading it through the Esp class would put aim state in the
//  same object as the ESP's camera and pool -- three concerns that are already
//  big enough.  If a second assist is ever wanted this becomes a struct; the
//  call sites do not change.
// -----------------------------------------------------------------------------

int   g_target_idx     = -1;      ///< index into last frame's candidate array
float g_target_x = 0.f, g_target_y = 0.f;
bool  g_have_target    = false;
bool  g_injected       = false;

/// The mouse position at the previous step, so "is the user moving" can be
/// answered.  Raw GetCursorPos deltas, not the game's: the physical pointer is
/// what moves, and this is a read of it.
POINT g_prev_cursor    = { 0, 0 };
DWORD g_prev_cursor_ms = 0;

/// Accumulated fractional remainder, so a per-frame sub-pixel correction is not
/// lost to integer rounding.  Without this, a 0.4 px/frame pull injects nothing
/// at all, every frame, forever.
float g_accum_x = 0.f, g_accum_y = 0.f;

/// Frames since the target was last seen, for the loss hysteresis.
int g_lost_frames = 0;

/// -------------------------------------------------------------------------
///  Is the user physically moving the mouse?
///
///  This is the gate that makes the assist proportional rather than a lock, and
///  it is the single most important condition in the file.  A soft assist that
///  corrects while the user is holding still is a lock with extra steps.
///
///  The measurement is the total displacement of the physical cursor over one
///  frame.  A tiny threshold filters out the 1-pixel jitter a mouse produces
///  when it is resting on a vibrating desk while still accepting a real, slow
///  drag.
/// -------------------------------------------------------------------------
bool user_is_moving_mouse() {
    POINT now{};
    GetCursorPos(&now);

    const DWORD t = GetTickCount();
    const float dx = (float)(now.x - g_prev_cursor.x);
    const float dy = (float)(now.y - g_prev_cursor.y);
    const float moved = std::sqrt(dx * dx + dy * dy);

    g_prev_cursor = now;

    // A frame that took longer than 100 ms is a stall, not a movement: the
    // engine was doing something else and the delta is meaningless.  Comparing
    // raw deltas across a stall would read a cursor teleport as a huge flick.
    const bool fresh = (t - g_prev_cursor_ms) < 100;
    g_prev_cursor_ms = t;

    if (!fresh) return false;
    return moved >= 1.5f;
}

/// Screen centre, the aim point.  The game's own crosshair sits here.
void aim_point(int sw, int sh, float& x, float& y) {
    x = sw * 0.5f;
    y = sh * 0.5f;
}

} // namespace

// =============================================================================
//  the step
// =============================================================================

bool aim_step(AimAssist& cfg,
              const AimCandidate* candidates, int count,
              int screen_w, int screen_h,
              bool enabled) {
    g_injected = false;

    // -------------------------------------------------------------------------
    //  Gates, in the order that fail fastest.
    //
    //  Every one of these is a hard stop, and the earlier a stop happens the
    //  better: the whole safety of the feature is that the list is exhaustive,
    //  so it is written to be readable as a list rather than folded into one
    //  expression.
    // -------------------------------------------------------------------------
    if (!enabled)                       { aim_reset(); return false; }
    if (!cfg.enabled)                   { aim_reset(); return false; }
    if (screen_w <= 0 || screen_h <= 0) { aim_reset(); return false; }

    // The activation key.  Mouse buttons are read the same way as keys --
    // GetAsyncKeyState takes the VK of a mouse button -- so one code path serves
    // both, and a mouse-button keybind needs nothing special.
    const bool key_down = (GetAsyncKeyState(cfg.vk_key) & 0x8000) != 0;
    cfg.key_down = key_down;

    if (!key_down) {
        // Not holding the key: no target, no injection, no state carried over.
        // Clearing the target here (rather than merely skipping) is what makes
        // the next press start clean, with no half-interpolated pull from the
        // last session.
        aim_reset();
        return false;
    }

    // The user must be moving the mouse.  This is the proportional gate.
    if (!user_is_moving_mouse()) {
        g_have_target = false;
        g_lost_frames = 0;
        return false;
    }

    // -------------------------------------------------------------------------
    //  Selection: the candidate closest to the aim point, within the radius.
    //
    //  Closest-to-crosshair rather than closest-in-world, because the assist
    //  acts in screen space and the screen-space distance is what the pull will
    //  actually travel.  A target behind a nearer one is still selectable if it
    //  is what the crosshair is on, and the radius is what stops that from
    //  reaching across the screen.
    // -------------------------------------------------------------------------
    float ax, ay;
    aim_point(screen_w, screen_h, ax, ay);

    int   best      = -1;
    float best_dist = cfg.radius_px;

    for (int i = 0; i < count; ++i) {
        const AimCandidate& c = candidates[i];
        if (!c.on_screen) continue;
        const float dx = c.x - ax, dy = c.y - ay;
        const float d  = std::sqrt(dx * dx + dy * dy);
        if (d < best_dist) { best_dist = d; best = i; }
    }

    if (best < 0) {
        // No target in range.  Hold the last one briefly, so a target that
        // flickers on and off the screen edge does not make the assist stutter,
        // then drop it.  Reset at the end of the window rather than at once.
        if (++g_lost_frames > 6) {
            g_have_target = false;
            g_target_idx  = -1;
        }
        return false;
    }

    g_lost_frames = 0;
    g_target_idx  = best;
    g_target_x    = candidates[best].x;
    g_target_y    = candidates[best].y;
    g_have_target = true;

    // -------------------------------------------------------------------------
    //  The correction: a fraction of the remaining pixel error.
    //
    //  error is (target - crosshair) in pixels.  The injected delta is
    //  error / smoothing, so a higher smoothing moves slower and a smoothing of
    //  1 is the fastest the slider allows.  The result is scaled down further
    //  the closer it is, so the pull weakens as the crosshair converges rather
    //  than snapping onto the target and staying: at 2 px of error the injected
    //  movement is a fraction of a pixel, which is the "ease in" that keeps it
    //  from feeling like a magnet.
    // -------------------------------------------------------------------------
    const float ex = g_target_x - ax;
    const float ey = g_target_y - ay;

    // Dead zone: inside a couple of pixels the assist stops entirely.  Without
    // it the crosshair would oscillate around the target forever as the
    // correction overshoots left and right by fractions of a pixel.
    if (std::fabs(ex) < 2.f && std::fabs(ey) < 2.f) return false;

    const float s = std::max(cfg.smoothing, 1.f);
    float dx = ex / s;
    float dy = ey / s;

    // A per-frame clamp, so a large error cannot produce a large jump.  The pull
    // is meant to be a nudge, and a nudge has a size limit independent of how
    // far away the target is.
    const float kMaxStep = 6.f;
    const float mag = std::sqrt(dx * dx + dy * dy);
    if (mag > kMaxStep) {
        dx = dx / mag * kMaxStep;
        dy = dy / mag * kMaxStep;
    }

    // Accumulate the sub-pixel remainder.
    g_accum_x += dx;
    g_accum_y += dy;
    const int ix = (int)g_accum_x;
    const int iy = (int)g_accum_y;
    if (ix == 0 && iy == 0) return false;
    g_accum_x -= (float)ix;
    g_accum_y -= (float)iy;

    // -------------------------------------------------------------------------
    //  The injection.
    //
    //  MOUSEEVENTF_MOVE with relative dx/dy is exactly what a mouse sends, and
    //  it is added to whatever the user's own movement was.  There is no
    //  MOUSEEVENTF_ABSOLUTE and no coordinate normalisation: those would make it
    //  an absolute set, which is the lock this feature is specifically not.
    // -------------------------------------------------------------------------
    INPUT in{};
    in.type           = INPUT_MOUSE;
    in.mi.dx          = ix;
    in.mi.dy          = iy;
    in.mi.dwFlags     = MOUSEEVENTF_MOVE;
    in.mi.time        = 0;
    in.mi.dwExtraInfo = 0;

    if (SendInput(1, &in, sizeof(INPUT)) == 1) {
        g_injected = true;
    }
    return g_injected;
}

bool aim_injected_last() { return g_injected; }

bool aim_current_target(float* x, float* y) {
    if (!g_have_target) return false;
    if (x) *x = g_target_x;
    if (y) *y = g_target_y;
    return true;
}

void aim_reset() {
    g_target_idx   = -1;
    g_have_target  = false;
    g_lost_frames  = 0;
    g_accum_x = 0.f;
    g_accum_y = 0.f;
    // The cursor baseline is NOT reset here on purpose: the next step compares
    // against where the pointer actually is, and re-baselining to a stale value
    // would read the whole intervening movement as one enormous flick.
}

} // namespace bs
} // namespace vex
