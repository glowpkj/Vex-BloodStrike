// -----------------------------------------------------------------------------
//  bs_esp.hpp  --  Blood Strike PC: player discovery by motion, and the ESP.
//
//  Everything this header declares rests on values measured against the running
//  process with a live, moving player.  None of them come from a published
//  dump, and two of the published ones are known to be wrong on this build:
//
//      ActorProps   published 0x3B8 and 0x48 -> actual 0x88
//      Position     published 0x1C0          -> actual 0x7C
//
//  Both wrong values produced plausible-looking numbers at least once, which is
//  why they survived so long.  See BLOODSTRIKE_ANALYSIS.md sections 0.1b and
//  0.3a for the measurements that displaced them.
//
//  Player discovery needs no entity container.  There is no reliable one: five
//  separate searches for it produced only false positives, and a reverse
//  reference sweep of the local player found 36 references, none of them in a
//  run of adjacent resolvable slots at any stride from 8 to 0x60.  Instead the
//  pool is found by what players are, not by where they are listed:
//
//      1. every mapped pointer in the process is collected          (~9.8 M)
//      2. each is tested for +0x3E0 == 2 and a finite world
//         position at +0x7C with magnitude > 5                       (~9 900)
//      3. the pool is polled twice a second; a candidate whose position
//         moves more than 0.5 units is promoted to a tracked actor
//
//  Step 2 alone is far too weak -- the field at +0x3E0 is not an entity class,
//  its distribution is nearly flat from 0 to 64.  Step 3 is what discriminates:
//  players move and scenery does not, and the rule is validated every cycle by
//  requiring the local player to still be among the movers.
// -----------------------------------------------------------------------------
#pragma once

#include <Windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>

#include "menu.hpp"
#include "overlay.hpp"
#include "sdk.hpp"

namespace vex {

namespace bs {

/// Measured on this build.  See BLOODSTRIKE_ANALYSIS.md section 0.3.
namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t Camera       = 0xD88;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t ActorInst    = 0x18;

    constexpr uint64_t ActorProps   = 0x88;   // not 0x3B8, not 0x48
    constexpr uint64_t Component    = 0x18;

    constexpr uint64_t CamPos       = 0x7C;
    constexpr uint64_t CamFov       = 0x454;
    constexpr uint64_t CamView      = 0x41C;  // 3x3 basis + translation, 48 bytes

    constexpr uint64_t EntPos       = 0x7C;   // not 0x1C0
    constexpr uint64_t EntField     = 0x3E0;  // pre-filter only, weak on its own

    /// Bone float3 array on the actor object: +0x208 sits at the root and
    /// +0x214 is about 2.2 units above it, which is head height.
    constexpr uint64_t BoneRoot     = 0x208;
    constexpr uint64_t BoneHead     = 0x214;
}

/// A tracked enemy.  Promoted from the pool by motion, then kept while it keeps
/// producing a sane world position.
struct Actor {
    uint64_t va   = 0;
    Vec3     pos{};              // root, from +0x7C
    Vec3     head{};             // from the bone array, when it reads sanely
    float    distance = 0.f;
    float    depth    = 0.f;     // camera-space forward distance, metres
    bool     on_screen = false;
    Vec2     screen_root{};
    Vec2     screen_head{};
    /// Smoothed screen position and size, for drawing.
    ///
    /// The read path runs at 120 ms while the draw path runs at 60 fps, so a
    /// raw screen position changes in steps and the box jerks from one place to
    /// the next -- reported as the boxes "stuttering as they update".  These
    /// follow the fresh value with a fixed fraction per frame instead, which is
    /// a low-pass filter: the box tracks the player without the steps.
    Vec2     smooth_root{};
    float    smooth_h      = 0.f;   // smoothed pixel height
    bool     smooth_valid  = false;
    uint64_t last_seen = 0;
    /// Monotonic time of the first failed read in the current run of failures,
    /// or 0 when the last read succeeded.
    ///
    /// Replaces a frame counter, which measured the wrong thing: "10 frames" is
    /// a different duration on every refresh rate.  A 500 ms window is the same
    /// on all of them and is what the decision actually wants.
    DWORD    stale_since = 0;
    uint32_t stale_frames = 0;
};

struct Settings {
    /// NOTE: the visual fields that used to live here (enabled, boxes, skeleton,
    /// radar, show_distance, max_distance, box_height) have moved to
    /// EspVisual in menu.hpp.  The reason is the menu: it edits one state
    /// object, and the renderer reads that same object, so a switch and the
    /// thing it controls can never disagree.  Keeping a second copy here is the
    /// classic way that breaks -- the menu edits one struct, the renderer reads
    /// the other, and the bug looks like "the switch does nothing".
    ///
    /// What remains are the discovery knobs, which the menu does not expose.
    ///
    /// Milliseconds between candidate-pool MERGE sweeps, or 0 to never sweep.
    ///
    /// A sweep reads about two gigabytes and blocks the drawing thread for forty
    /// to sixty seconds, so this is a straight trade between two bad options and
    /// the number is chosen deliberately:
    ///
    ///   too large  a player who spawns after the first sweep is invisible for
    ///              that long.  This is the bug that made the ESP look broken in
    ///              every match that had already started: the pool was built at
    ///              the first live position, when the map was nearly empty, and
    ///              never extended.  The log showed "2 candidates kept" and
    ///              "tracked 2" for a whole match.
    ///
    ///   too small  the overlay freezes every N seconds, which is the stall that
    ///              was reported as "the boxes stop and then come back".
    ///
    /// A merge -- not a replacement -- is what makes the second option survivable
    /// at all: a merge keeps every watched address and its movement history, so a
    /// sweep is additive rather than destructive.  See build_pool's merge
    /// documentation.
    ///
    /// 20000 is chosen now that the sweep is off the drawing thread.  The only
    /// remaining cost of a short interval is CPU: a sweep reads two gigabytes, so
    /// at twenty seconds a background thread is scanning much of the time, and
    /// the game shares those pages with it.  The benefit is that a player who
    /// spawns mid-match becomes visible within twenty seconds instead of never.
    ///
    /// 0 disables the periodic sweep entirely, and every sweep then comes from
    /// the one-shot at match entry -- which is the configuration that made the
    /// ESP look broken, so it is available and not recommended.
    int   pool_refresh_ms = 20'000;
    int   scan_quarter  = 1;     // 1 = poll the whole pool each cycle
};

class Esp {
public:
    /// Verify the projection and exit, instead of running the overlay loop.
    /// Needed because the normal path only quits on two ESC presses.
    bool selftest_ = false;

    /// Full read pipeline, no window at all.
    ///
    /// Exists to answer one question that has now cost several runs: is the
    /// game's broken mouse input caused by the overlay WINDOW, or by the reading
    /// process itself?  With no window created, the answer is unambiguous -- if
    /// the game's mouse works now, the window is the cause; if it does not, the
    /// window is innocent and something in the process is interfering.
    bool nooverlay_ = false;

    /// Present the overlay through UpdateLayeredWindow (--ulw) instead of a
    /// DXGI swapchain.
    ///
    /// The two paths have different measured properties and one had to be chosen
    /// by evidence rather than by reading: the swapchain path draws the boxes
    /// reliably but routes no mouse, and a layered window routes the mouse but
    /// has no content of its own.  This flag selects the layered path, which
    /// supplies that content from the CPU, and the frame cost it pays for it is
    /// printed.
    bool ulw_ = false;

    /// Run forever, latching onto a match whenever one starts (--persist).
    ///
    /// The default behaviour used to be to give up and exit if the local player
    /// had no world position within sixty seconds, which meant the tool had to be
    /// launched during a match and re-launched after every trip to the menu, and
    /// each launch cost the fifty-second pool sweep before anything appeared.
    /// With this set, start() only requires that the game process and the chain
    /// resolve; the overlay comes up immediately and shows nothing while the
    /// player is in a menu, dead, or spectating, and begins drawing the moment a
    /// live position appears.  The process then stays up until killed.
    bool persist_ = true;

    bool start();
    void run();
    void stop();

    /// Drive discovery for a few seconds without drawing anything, then print
    /// what it found.  Exists because the ESP's real question -- are there
    /// enemies in the pool? -- cannot be answered by looking at a screenshot,
    /// and a screenshot that shows nothing is not evidence either way.
    void observe(int seconds);

private:
    // --- chain ------------------------------------------------------------
    bool resolve_chain();
    void trace_chain() const;

    // --- discovery --------------------------------------------------------
    /// Sweep the address space for candidate actors.
    ///
    /// `merge` false replaces the pool -- the original behaviour, correct for
    /// the initial sweep before any actor exists.
    ///
    /// `merge` true EXTENDS it, keeping every address already being watched and
    /// its movement history, and appending only what is new.  That is what makes
    /// a periodic rebuild safe: a match fills up with players after the first
    /// sweep, and without a merge they are never found.  See the comment on the
    /// definition for the run that established this.
    void build_pool(bool merge = false);
    void poll_pool();
    void rescan_if_due();
    void validate_pool();

    // --- per-frame --------------------------------------------------------
    void update_actors();
    void project();

    // --- drawing ----------------------------------------------------------
    void draw_frame();
    void draw_esp();
    void draw_radar();
    void draw_hud();
    void draw_menu(bool* open);

    /// Project a world point to screen pixels.
    ///
    /// out_depth, when given, receives the camera-space depth in metres.  It
    /// exists because scaling a world-space height -- the box height fallback --
    /// into pixels needs the depth, and the caller cannot recompute it without
    /// reading the camera a second time and getting a different frame's value.
    bool world_to_screen(const Vec3& world, Vec2& out, float* out_depth = nullptr);
    bool read_view(float m[12]) const;

    /// Decide which direction along row2 of the camera basis counts as forward.
    ///
    /// Called once per frame from update_actors(), before anything projects.  It
    /// has to run outside world_to_screen(): that function is called per actor,
    /// and a decision that changes halfway through a frame draws half the boxes
    /// with one sign and half with the other.
    void measure_forward_sign();

    // --- input ------------------------------------------------------------
    bool key_once(int vk);
    void pump_keys();

    // state
    Overlay     overlay_;
    Settings    cfg_{};

    /// The one configuration state, shared with the menu.  The menu writes it,
    /// this class reads it.  See menu.hpp for why it is split into esp/aim/menu.
    UiState     ui_{};

    bool        running_ = false;
    bool        menu_open_ = false;
    /// Whether the UI font has been loaded.  Loading is deferred until after the
    /// overlay has created the ImGui context and the D3D device, because
    /// AddFontFromFileTTF before CreateContext() would have nowhere to go.
    bool        font_loaded_ = false;

    /// Whether the overlay window is currently shown.  The overlay is hidden
    /// whenever the game is not the foreground window, so that alt-tabbing does
    /// not leave a topmost black rectangle over the desktop that cannot be
    /// clicked away.
    bool        overlay_shown_ = false;

    /// Tick at which the game window was first noticed missing, or 0 while it is
    /// present.  Used to shut the overlay down a few seconds after the game
    /// exits, rather than leaving a topmost window with nothing to draw over.
    DWORD       game_gone_since_ = 0;

    // chain
    uint64_t base_ = 0;
    uint64_t engine_ = 0, gameplay_ = 0, client_ = 0;
    uint64_t camera_ = 0, local_ = 0, actor_instance_ = 0;

    /// The game's top-level window, found once and reused.
    ///
    /// game_window() enumerates every top-level window on the desktop, and it
    /// was being called once per frame to read the client size.  Caching it here
    /// takes a full desktop enumeration out of the render loop; it is refreshed
    /// only when the handle stops being a window.
    HWND     game_hwnd_ = nullptr;

    // camera
    Vec3    cam_pos_{};
    float   cam_fov_ = 90.f;
    float   view_[12]{};
    int     screen_w_ = 0, screen_h_ = 0;
    bool    view_ok_ = false;

    // pool
    std::vector<uint64_t> pool_;

    /// Movement events per candidate address.  One event is not evidence and is
    /// not treated as such: a single movement was measured on 150+ pieces of
    /// scenery in one firing-range run, all of it caused by the camera sweeping
    /// rather than by anything moving.  Promotion needs two events close
    /// together at the same address.
    struct MoverHits { int count = 0; DWORD last = 0; };

    std::unordered_map<uint64_t, Vec3>      last_pos_;
    std::unordered_map<uint64_t, MoverHits> mover_hits_;

    /// Where each candidate that has ever moved was last seen, and how far it
    /// has travelled in total.  Kept for candidates that did NOT get promoted,
    /// which is the case that needs explaining: 11 objects moving and zero
    /// promoted is either a real engine behaviour or a broken rule, and the
    /// positions say which.
    struct MoverTrace { Vec3 pos{}; float travelled = 0.f; int events = 0; };
    std::unordered_map<uint64_t, MoverTrace> mover_trace_;
    std::unordered_map<uint64_t, Actor>     actors_;
    size_t   rejected_static_ = 0;   ///< dropped by the shape rules in build_pool

    /// Where every candidate poll exits, counted.
    ///
    /// A run with a full pool and zero promotions cannot be diagnosed from the
    /// verdict alone: "0 moved" and "0 promoted" are the same line whether the
    /// exit is the position test, the distance test, the still test, or the
    /// two-event rule.  Those need four different fixes.  This says which.
    size_t diag_polls_ = 0, diag_noread_ = 0, diag_implausible_ = 0,
           diag_toosmall_ = 0, diag_first_ = 0, diag_still_ = 0,
           diag_moved_ = 0, diag_evt1_ = 0, diag_evt2_ = 0, diag_nearcam_ = 0;

    /// Why a mover that passed the motion test was still not promoted.
    ///
    /// The motion test alone was promoting map geometry, and the failure was
    /// invisible: the object simply appeared in the tracked set with a position
    /// that made no sense, and the only symptom was "no boxes".  Splitting the
    /// rejection into three counters makes the reason a number in the log --
    /// no bones at all, bones that are not a human shape, or bones belonging to
    /// a different object than the position field.  The three need different
    /// fixes, so they are worth counting separately.
    size_t diag_nobone_ = 0, diag_boneshape_ = 0, diag_farroot_ = 0;

    /// The same three counts, taken during pool construction rather than at
    /// promotion.  They are separate members because they answer a different
    /// question: these say how much of the pool was scenery, and a large number
    /// here with a small one in the diag_ set means the filter is working at the
    /// right stage.
    size_t pool_nobone_ = 0, pool_boneshape_ = 0, pool_farroot_ = 0;

    /// Whether the local player's own address survived the shape rules and made
    /// it into the candidate pool.  Printed by observe(): "not moved" and "never
    /// even considered" produce the same verdict line otherwise, and they need
    /// completely different fixes.
    bool     local_in_pool_ = false;

    /// Every address that has been the local player during this session.
    ///
    /// local_ is re-resolved every frame from the chain, and the chain's target
    /// object changes across a respawn or a round transition -- measured twice
    /// this session, two different addresses for the same seat.  Comparing the
    /// pool against the single live local_ therefore fails for the rest of the
    /// session after any such change, silently, which is what "in pool: yes"
    /// together with "0 events" was actually reporting.
    ///
    /// In a deathmatch this happens every few seconds, so a single stored
    /// address is not enough: the set grows, and an actor is recognised as the
    /// local player if it matches any member.
    std::vector<uint64_t> local_addrs_;

    bool is_local(uint64_t va) const {
        for (uint64_t a : local_addrs_) if (a == va) return true;
        return false;
    }
    uint64_t last_pool_build_ = 0;
    size_t   pool_cursor_ = 0;
    size_t   candidate_total_ = 0;
    size_t   movers_seen_ = 0;      ///< candidates with at least one movement event
    bool     pool_built_ = false;
    bool     local_is_mover_ = false;

    // -----------------------------------------------------------------------
    //  The background sweep.
    //
    //  WHY IT EXISTS
    //
    //  A match is found, then it fills up.  A sweep that runs when the match
    //  begins therefore sees almost nobody -- the log of 2026-10-17 said "1
    //  candidates kept" at the moment of entry -- and every player who spawns
    //  after it is invisible until the next sweep.  Making the next sweep happen
    //  sooner is not an option while it runs on the drawing thread: a sweep reads
    //  two gigabytes and blocks for forty to sixty seconds, so at any useful
    //  interval the overlay is a slideshow.
    //
    //  So the sweep runs on this thread instead.  It walks the address space
    //  exactly as before and builds its own candidate list; the drawing thread
    //  never blocks and never sees a half-built pool.  When the sweep finishes it
    //  takes pool_mtx_ for the few microseconds it takes to merge its result into
    //  pool_ and last_pos_, which is the only point at which the two threads
    //  touch the same data.
    //
    //  The merge rules are unchanged: an address already being watched keeps its
    //  place and its movement history, only genuinely new addresses are appended,
    //  and the cursor is re-armed because an index into a re-sorted array means
    //  nothing.  See build_pool's merge documentation for why that is the correct
    //  semantics rather than a convenience.
    //
    //  LIFETIME
    //
    //  The worker is joined in stop(), before the process handle is closed, so it
    //  can never read from a torn-down process or write to a destroyed Esp.  It
    //  is also demand-driven rather than on a timer: it waits on an event that
    //  rescan_if_due() signals, so a sweep that takes ninety seconds delays only
    //  the NEXT sweep, never a second one running concurrently.
    // -----------------------------------------------------------------------
    void sweep_thread_main();
    /// Signal the worker that a merge sweep is due.
    void request_sweep();
    /// True while a sweep is in flight, for the status line.
    bool sweep_running() const;
    /// The thread entry point.  Static because CreateThread takes a plain
    /// function pointer and cannot call a member: the `this` is passed through
    /// lpParameter and re-enters the class here.
    static DWORD WINAPI sweep_thread_entry(LPVOID self);
    /// Take whatever the worker posted and merge it into the pool.
    ///
    /// Called from poll_pool, on the drawing thread, which is the only thread
    /// that writes pool_ and last_pos_.  See the definition for why the merge
    /// happens here rather than in the worker.
    void merge_sweep_inbox();

    HANDLE          sweep_thread_  = nullptr;
    HANDLE          sweep_event_   = nullptr;   ///< manual-reset; signalled to start
    HANDLE          sweep_stop_    = nullptr;   ///< manual-reset; signalled to exit
    std::mutex      pool_mtx_;                  ///< guards sweep_inbox_ ONLY
    std::vector<uint64_t> sweep_inbox_;         ///< addresses found by the worker
    bool            sweep_inbox_ready_ = false; ///< true when sweep_inbox_ has data
    volatile LONG   sweep_active_  = 0;         ///< 1 while a sweep is in flight
    volatile LONG   sweep_pending_ = 0;         ///< 1 when a request arrived mid-sweep

    // stats
    DWORD  last_poll_ = 0;
    DWORD  last_tick_ = 0;
    int    fps_ = 0;
    int    frames_ = 0;
    DWORD  fps_tick_ = 0;

    /// Sign of the camera's forward axis in the 3x3 basis.
    ///
    /// MEASURED, not voted.  See measure_forward_sign() for why the vote that
    /// used to choose this was removed: it was correct in one match and wrong in
    /// the next with no code change, because its score is fooled by actors behind
    /// the camera under the wrong convention.
    int    fwd_sign_ = 0;

    /// The basis at +0x41C is stored COLUMN-major and must be read column-wise.
    ///
    /// MEASURED, by dumping the twelve floats: row1 is the vertical axis under
    /// the column reading (+/-0.0004, 0.988, 0.155) and is not vertical under the
    /// row reading, which is what disambiguates them.  Committed as a constant so
    /// the projection cannot silently invert between runs.
    bool   col_major_ = true;

    /// The committed forward sign for this build, from the same measurement.
    static constexpr int kForwardSign = -1;
    static constexpr bool kColMajor   = true;

    /// Set once the diagnostic vote has run, so it is reported once per match.
    bool   fwd_voted_ = false;

    /// True while the local player has no usable world position.
    ///
    /// The read path keeps running in this state -- the pool is built, the chain
    /// is re-resolved -- but nothing is drawn, because there is nothing valid to
    /// draw and a stale camera projects garbage.  It clears the moment a live
    /// position appears, which is the menu -> match transition.
    bool   waiting_for_match_ = true;
    /// Tick of the last "still waiting" report, so the log is a heartbeat and
    /// not a flood.
    DWORD  wait_report_ms_ = 0;
    /// Tick at which the local player first had no world position in the current
    /// run of such frames, or 0 when the last frame had one.  Used with
    /// hysteresis to decide a match has ended -- see run().
    DWORD  lost_since_ = 0;

    /// Last fov that looked like ordinary gameplay, in degrees.
    ///
    /// The camera fov is NOT constant during a match.  Read live from the log:
    /// it sits at 112 in normal play and jumps to 60, 74.6, 31.3, 23.0, 20.1 and
    /// similar whenever the player scopes, enters a killcam, or spectates.  Each
    /// jump rescales every box, because the pixel scale is
    /// (screen_w * 0.5) / tan(fov * 0.5) -- at fov 20 the same enemy projects
    /// several times larger than at 112.
    ///
    /// That is the whole of "sometimes it bugs out mid-match" and of "when I die
    /// it stops working and then comes back": on death the camera switches to the
    /// killcam, which runs a narrow fov, and every box is drawn at the wrong size
    /// and the wrong place until respawn restores 112.
    ///
    /// The fov is still read live -- it has to be, because a genuine scope does
    /// change it -- but this holds the last value that was in the gameplay band,
    /// which is what is used to scale boxes.
    float  fov_stable_ = 112.f;
    /// Frames in a row with an out-of-band fov.  Used to decide the camera is in
    /// a killcam or spectator state rather than simply looking through a scope,
    /// and to blank the drawing until it returns.
    int    fov_odd_frames_ = 0;

    // keys
    bool k_toggle_ = false, k_menu_ = false, k_box_ = false,
         k_skel_ = false, k_radar_ = false, k_exit_ = false;
    DWORD exit_armed_ = 0;
};

} // namespace bs
} // namespace vex