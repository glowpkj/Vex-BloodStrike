// -----------------------------------------------------------------------------
//  aim.hpp  --  proportional aim assist.
//
//  WHAT THIS IS
//
//  A soft assist: while the activation key is held AND the user is physically
//  moving the mouse, it adds a small corrective movement toward the selected
//  target.  It never takes over.  Release the key, stop moving, or lose the
//  target, and it does absolutely nothing -- the user's own input is the only
//  input unless every one of those conditions holds at once.
//
//  WHY IT IS BUILT THIS WAY
//
//  Three integration points were available and two were rejected, on the same
//  grounds in both cases: they are absolute, and absolute is a lock.
//
//    * Writing the camera angle into game memory.  Rejected explicitly, and
//      rejected here too.  An absolute write is a hard lock, it is visible as a
//      discontinuity, and it removes the operator from the loop -- which is both
//      worse at the game and worse to explain.
//
//    * Injecting an absolute mouse position.  Same objection: one event that
//      teleports the view is exactly what it looks like.
//
//    * Injecting PROPORTIONAL RELATIVE MOTION.  What is done here.  It is the
//      same mechanism a physical mouse produces -- a relative delta -- so it is
//      not a new class of input to the game, and because the magnitude is a
//      fraction of the remaining error and is gated on the user already moving,
//      it reads as a mouse that is easier to control rather than as anything
//      snapping into place.
//
//  WHAT IT DEPENDS ON
//
//  Nothing new.  The camera basis at +0x41C is already read every frame for the
//  ESP, and the caller projects each candidate target to screen with the same
//  world_to_screen the boxes use.  This module therefore receives SCREEN-SPACE
//  POINTS and does not know what an Actor is -- which is deliberate, and has two
//  consequences worth stating: the assist can never be more accurate than the
//  projection (so building it on anything less verified would be wrong), and the
//  module can be tested against synthetic points with no game running at all.
//
//  WHAT IS DELIBERATELY NOT HERE
//
//  No driver, no evasion, no hook, no anti-cheat interaction of any kind.  The
//  module calls one Windows function (SendInput) and reads nothing it was not
//  already reading.
// -----------------------------------------------------------------------------
#pragma once

#include <Windows.h>
#include <cstdint>

namespace vex {
namespace bs {

struct AimAssist;   // defined in menu.hpp

/// One candidate target, in overlay client pixels.
struct AimCandidate {
    float x = 0.f;
    float y = 0.f;
    float distance = 0.f;   ///< metres, for the range gate
    bool  on_screen = false;
};

/// One assist step.  Call once per frame, after the actors have been projected.
///
/// `candidates` is an array of `count` screen-space points, already filtered by
/// the caller to exclude the local player.  The module picks the one nearest the
/// aim point and works on it.
///
/// `enabled` must already fold in every gate that lives outside this module: the
/// master switch, a live local player, the game being in the foreground, and a
/// valid projection.  This function adds the gates it owns -- the activation key,
/// the requirement that the user is actually moving the mouse, and the selection
/// radius -- on top of that.
///
/// Returns true if a correction was injected this frame.
bool aim_step(AimAssist& cfg,
              const AimCandidate* candidates, int count,
              int screen_w, int screen_h,
              bool enabled);

/// True if the assist injected on the most recent step.
bool aim_injected_last();

/// The target the most recent step selected, in client pixels, or false if none.
bool aim_current_target(float* x, float* y);

/// Reset the per-session state.  Called on menu close, on match exit and on
/// toggle, so a latched key or a stale target cannot survive a transition.
void aim_reset();

} // namespace bs
} // namespace vex
