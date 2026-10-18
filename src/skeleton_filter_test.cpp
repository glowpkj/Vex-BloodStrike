// -----------------------------------------------------------------------------
//  skeleton_filter_test.cpp  --  verify the pool's player/scenery decision.
//
//  This exists because the failure it guards against is invisible from outside.
//  A pool that keeps 6,710 pieces of scenery and a pool that keeps 6,710
//  players both print "6710 candidates kept"; the difference only shows up
//  three seconds later as "no boxes", on somebody else's screen, in a match.
//  So the decision is made computable here, and the two cases are the two real
//  objects from the 2026-10-17 run, taken verbatim from the log.
//
//  Case A -- an actor. +0x7C is the feet, +0x208 the skeleton root a hand's
//            width below the head, +0x214 the head 2.2 units up.  All three
//            agree, because they are three fields of one object.
//
//  Case B -- the object that was actually promoted and drawn nothing. The log
//            printed it as:
//                pos.y 65.201  root.y -0.000   (pos-root 65.423)
//            A pinned y that never varied across a whole match, and a root
//            65 m away from the feet: two fields of two different objects.
//
//  A filter that accepts A and rejects B is the filter that turns the second
//  run into boxes.  Anything else is a filter that has not been tested.
// -----------------------------------------------------------------------------
#include <cmath>
#include <cstdio>

namespace {

struct V3 { float x, y, z; };

/// The decision exactly as build_pool implements it.
///
/// Kept as a copy rather than linked, because build_pool is a member of Esp and
/// pulls in the whole read path; the arithmetic is four lines and is the thing
/// being tested.  If the two ever diverge, the divergence shows up in the live
/// pool counts, which print the same three rejections this counts.
bool looks_like_an_actor(const V3& feet, const V3& root, const V3& head) {
    const float up   = head.y - root.y;
    const float span = std::sqrt((head.x - root.x) * (head.x - root.x) +
                                 (head.y - root.y) * (head.y - root.y) +
                                 (head.z - root.z) * (head.z - root.z));
    if (!(up > 0.8f && up < 2.6f && span < 3.f)) return false;

    const float rdx = root.x - feet.x, rdy = root.y - feet.y, rdz = root.z - feet.z;
    if (std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz) > 1.5f) return false;

    return true;
}

int fails = 0;

void expect(bool ok, const char* what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

} // namespace

int main() {
    printf("skeleton filter -- player vs scenery\n");

    // -------------------------------------------------------------------------
    //  Case A: an actor.  Feet on the ground, root just above them, head 2.2 up.
    // -------------------------------------------------------------------------
    {
        const V3 feet{ 120.0f, 0.00f, -80.0f };
        const V3 root{ 120.0f, 0.15f, -80.0f };
        const V3 head{ 120.0f, 2.35f, -80.0f };
        printf("\n  A -- actor (log: a real player, feet/root/head agreeing)\n");
        expect(looks_like_an_actor(feet, root, head), "accepted");
    }

    // -------------------------------------------------------------------------
    //  Case B: the object from the log that was promoted and drew nothing.
    //  pos.y was pinned at 65.201; the root read as 0.000.
    // -------------------------------------------------------------------------
    {
        const V3 feet{ 210.0f, 65.201f, -179.0f };
        const V3 root{ 210.0f,  0.000f, -179.0f };
        const V3 head{ 210.0f,  0.000f, -179.0f };
        printf("\n  B -- the promoted non-actor (log: pos.y 65.201, root.y -0.000)\n");
        expect(!looks_like_an_actor(feet, root, head), "rejected");
    }

    // -------------------------------------------------------------------------
    //  Case C: scenery with no bone pair at all.  +0x208 is zeroes, which is
    //  what most of the pool is -- a door has no skeleton and reads as (0,0,0).
    // -------------------------------------------------------------------------
    {
        const V3 feet{ 300.0f, 12.0f, 40.0f };
        const V3 root{ 0.0f, 0.0f, 0.0f };
        const V3 head{ 0.0f, 0.0f, 0.0f };
        printf("\n  C -- scenery, bone slot reads as zeroes\n");
        expect(!looks_like_an_actor(feet, root, head), "rejected");
    }

    // -------------------------------------------------------------------------
    //  Case D: a bone pair that agrees with itself but not with the feet --
    //  the most dangerous false positive, because it looks like a skeleton.
    //  This is a different object's bones read through a wrong offset.
    // -------------------------------------------------------------------------
    {
        const V3 feet{ 100.0f, 0.0f, 100.0f };
        const V3 root{ 400.0f, 0.0f, 400.0f };    // a valid human shape...
        const V3 head{ 400.0f, 2.2f, 400.0f };    // ...300 m from the feet
        printf("\n  D -- valid bone shape, feet 300 m away (wrong-object bones)\n");
        expect(!looks_like_an_actor(feet, root, head), "rejected");
    }

    // -------------------------------------------------------------------------
    //  Case E: the tolerances.  A crouch moves the root, and the filter must
    //  not reject a crouching player -- which is the false negative that would
    //  cost more than it saves, because a crouching enemy is exactly the one
    //  worth seeing.
    // -------------------------------------------------------------------------
    {
        const V3 feet{ 50.0f, 0.0f, 50.0f };
        const V3 root{ 50.0f, 0.4f, 50.0f };      // crouched, root low
        const V3 head{ 50.0f, 1.7f, 50.0f };      // head lower than standing
        printf("\n  E -- crouching player (root low, head low)\n");
        expect(looks_like_an_actor(feet, root, head), "accepted");
    }

    // -------------------------------------------------------------------------
    //  Case F: the boundary.  root 1.4 m from the feet must pass, 1.6 m must
    //  not, so the threshold is where it is claimed to be rather than wherever
    //  the comparison happens to land.
    // -------------------------------------------------------------------------
    {
        const V3 feet{ 0.0f, 0.0f, 0.0f };
        printf("\n  F -- the root-to-feet threshold, both sides\n");
        expect(looks_like_an_actor(feet, { 0.0f, 1.4f, 0.0f }, { 0.0f, 3.6f, 0.0f }),
               "1.4 m from the feet: accepted");
        expect(!looks_like_an_actor(feet, { 0.0f, 1.6f, 0.0f }, { 0.0f, 3.8f, 0.0f }),
               "1.6 m from the feet: rejected");
    }

    printf("\nskeleton_filter_test: %d failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
