// -----------------------------------------------------------------------------
//  bs_esp.cpp  --  Blood Strike PC: player discovery by motion, and the ESP.
//
//  Design notes that are not obvious from the code:
//
//  Memory is read with plain OpenProcess + ReadProcessMemory.  A kernel driver
//  was considered and rejected on evidence: this build was read end to end,
//  including 1.1 GB, through the usermode handle.  Every offset here was found
//  that way, so a driver would add a detection surface and buy nothing.
//
//  There is no entity list.  Five searches for one produced only false
//  positives, and a reverse reference sweep of the local player found 36
//  references with none of them in a run of adjacent resolvable slots at any
//  stride from 8 to 0x60.  Players are therefore found by motion rather than
//  by enumeration:
//
//      pointer sweep        -> ~9.8 M candidates
//      signature filter     -> ~9 900 candidates
//      motion filter        -> the handful that are actually players
//
//  The signature filter is weak on its own and is treated as such: the field at
//  +0x3E0 is not an entity class, it is nearly flat across 0..64 over the whole
//  process.  The motion filter is what makes the result trustworthy, and it is
//  validated every cycle by requiring the local player to still be present.
//
//  The world-to-screen projection uses the 3x3 orthonormal basis at Camera+0x41C
//  as the camera-space basis and the FOV for the focal length.  Rather than
//  assume a sign convention for the forward axis, the sign is auto-detected
//  once against the local player, whose root sits directly under the camera and
//  therefore has to project near the centre of the screen.  A projection that
//  puts the local player at a corner is wrong, and saying so is better than
//  drawing boxes in the wrong place.
// -----------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"

#include "bs_esp.hpp"
#include "aim.hpp"
#include "menu.hpp"
#include "overlay.hpp"
#include "stealth.hpp"

#include <cstdarg>
#include <share.h>

namespace {

FILE* g_log = nullptr;
CRITICAL_SECTION g_log_cs;
bool  g_console_ok = false;

/// Write one line to the log file, and to the console when one is safely usable.
///
/// The file is written FIRST and the console second, and the console write is
/// gated on g_console_ok rather than on `stdout != nullptr`.  That gate is not
/// paranoia: `stdout` is a pointer to a FILE structure and is never null, so the
/// earlier `if (stdout)` test passed even when the stream's underlying handle was
/// invalid, and fwrite on it could take the process down.  That is what an
/// elevated launch with a hidden console produced -- a log file created and left
/// at zero bytes, with the process dying before its first line, which reads
/// exactly like a crash in the startup logic and was a crash in the logging.
///
/// So: the file is the sink, unconditionally.  The console is a bonus, taken only
/// once AllocConsole has reported success and the stream has been proven by use.
void logf(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (n <= 0) return;

    EnterCriticalSection(&g_log_cs);

    // Up to two buffers: one main, one spill for the rare long line.  Kept as
    // fixed arrays so nothing allocates on a path that must not fail.
    static char stackbuf[4096];
    char* buf = stackbuf;
    static char spill[16384];
    if (n >= 4096) { buf = spill; if (n > 16383) n = 16383; }

    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf[n] = '\0';

    if (g_log) { fwrite(buf, 1, (size_t)n, g_log); fflush(g_log); }

    if (g_console_ok && stdout) {
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }

    LeaveCriticalSection(&g_log_cs);
}

} // namespace

// Every printf in this translation unit goes to the durable, per-line-flushed
// logger as well.  A macro rather than an edit of several hundred call sites:
// the call sites are correct, it is only the sink that was wrong.  It must be
// defined here, above every function that prints -- this file's own functions
// live below it, so the macro reaches all of them.
#define printf vex_log_printf
#define vex_log_printf(...) logf(__VA_ARGS__)

namespace vex {

// stealth.cpp asks the kernel-mapped driver whether its protection is live.
// This build reads through a plain usermode handle -- the whole offset set was
// found that way, including 1.1 GB of game memory -- so there is no driver, and
// the honest answer is that kernel protection is not active.  Providing the
// symbol here keeps the driver loader and its signed-binary path out of a
// target that does not need either.
namespace kmm {
bool ready() { return false; }
} // namespace kmm

namespace bs {

namespace {

// -----------------------------------------------------------------------------
//  Fov band for the states the player is actually aiming from.
//
//  Measured from a live match log: normal play reads 112, ordinary scopes read
//  60-85, and the values seen only on death or in spectator -- 19.4, 20.1, 23.0,
//  31.3 -- are the killcam's.  The lower bound sits below every scope value
//  observed and above every killcam value, so no real gameplay state is excluded
//  and no killcam state is included.
//
//  A frame count rather than an instant switch, because a torn read of the fov
//  field produces exactly one wild value and must not blank the screen.
// -----------------------------------------------------------------------------
constexpr float kFovGameplayLo  = 40.f;
constexpr float kFovGameplayHi  = 130.f;
constexpr int   kFovOddFramesToBlank = 3;

HANDLE   g_h = nullptr;
DWORD    g_pid = 0;
uint64_t g_base = 0;
std::vector<MEMORY_BASIC_INFORMATION> g_regions;
DWORD    g_last_build = 0;

bool find_process() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(s, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, L"BloodStrike.exe")) { g_pid = pe.th32ProcessID; break; }
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    if (!g_pid) return false;

    if (g_h) { CloseHandle(g_h); g_h = nullptr; }
    g_h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, g_pid);
    if (!g_h) return false;

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, g_pid);
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(s2, &me)) {
        do {
            if (!_wcsicmp(me.szModule, L"BloodStrike.exe")) { g_base = (uint64_t)me.modBaseAddr; break; }
        } while (Module32NextW(s2, &me));
    }
    CloseHandle(s2);
    if (!g_base) return false;

    g_regions.clear();
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uintptr_t a = 0;
    while ((uint64_t)a < (uint64_t)si.lpMaximumApplicationAddress) {
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQueryEx(g_h, (LPCVOID)a, &m, sizeof m) == 0) break;
        if (m.State == MEM_COMMIT && m.RegionSize >= 8 &&
            !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS | PAGE_EXECUTE)))
            g_regions.push_back(m);
        uintptr_t nx = (uintptr_t)m.BaseAddress + m.RegionSize;
        if (nx <= a) break;
        a = nx;
    }
    std::sort(g_regions.begin(), g_regions.end(),
              [](const auto& x, const auto& y) {
                  return (uint64_t)x.BaseAddress < (uint64_t)y.BaseAddress; });
    g_last_build = GetTickCount();
    return true;
}

inline bool read(uint64_t va, void* dst, size_t n) {
    if (!g_h || va == 0) return false;
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_h, (LPCVOID)va, dst, n, &got)) return false;
    return got == n;
}
inline uint64_t r64(uint64_t va) { uint64_t v = 0; read(va, &v, 8); return v; }
inline uint32_t r32(uint64_t va) { uint32_t v = 0; read(va, &v, 4); return v; }

/// Chunk size for the address-space walk.  Shared by the synchronous sweep and
/// the background one so that both read the process in the same granularity: a
/// sixteen-megabyte buffer is large enough that the number of ReadProcessMemory
/// calls is bounded and small enough that it does not itself become the cost.
constexpr size_t CH = 16u << 20;
inline float    rf(uint64_t va) { float    v = 0; read(va, &v, 4); return v; }

inline bool mapped(uint64_t va) {
    if (va < 0x10000 || va > 0x00007FFFFFFFFFFFull) return false;
    size_t lo = 0, hi = g_regions.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint64_t a = (uint64_t)g_regions[mid].BaseAddress;
        uint64_t b = a + g_regions[mid].RegionSize;
        if (va < a) hi = mid;
        else if (va >= b) lo = mid + 1;
        else return true;
    }
    return false;
}

struct Vec3f { float x = 0, y = 0, z = 0; };

inline bool read_pos(uint64_t va, uint64_t off, Vec3f& p) {
    if (!read(va + off, &p, 12)) return false;
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
           std::fabs(p.x) < 1e5f && std::fabs(p.y) < 1e5f && std::fabs(p.z) < 1e5f;
}

inline float mag3(const Vec3f& p) {
    return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
}

/// The shape rules that say "this is not a location a person occupied".
///
/// Shared by both filters below.  local_ is exempt from the ground rule because
/// this engine reports the local actor's root on the ground plane -- measured at
/// y == 0.00 twice, at (-32.42, 0.00, -12.41) and (-18.42, 0.00, 15.27).
inline bool shape_ok(const Vec3f& p, bool is_local_player) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        return false;
    if (mag3(p) < 5.f)          return false;   // spawn point / origin noise
    int zero_axes = (p.x == 0.f) + (p.y == 0.f) + (p.z == 0.f);
    if (zero_axes >= 2)         return false;   // scalar with padding beside it
    if (p.x == p.z && p.x != 0.f) return false; // replicated template shape
    if (p.y == 0.f && !is_local_player) return false;
    return true;
}

/// The permissive filter, used while building the candidate pool.
///
/// Deliberately the original three rules and nothing more.  Adding the range
/// test here dropped the pool from 1,517 candidates to 302 -- an 80% cut -- and
/// with it every real actor, because at scan time an address can be read during
/// a transient state and a strict test discards it permanently.  The scan is
/// looking for addresses worth *watching*; it does not need to certify them.
inline bool scan_plausible(const Vec3f& p, bool is_local_player) {
    return shape_ok(p, is_local_player);
}

/// The strict filter, applied to every poll before a promotion.
///
/// This is where the range checks belong, and this is where the bug was.  A run
/// promoted 0x0000021E11116960 at (0.00, -9295.86, -142.95),
/// 0x00000030FEEFB250 at (0.00, 14.09, 14.09) -- not even a heap address of the
/// game, and a mirrored template the shape rule should have caught -- and
/// 0x0000021E47593904 at (55612.56, 0.00, 55608.06).  Nine thousand metres away,
/// mirrored, and fifty-five thousand metres away.  Those are impossible
/// positions for a player and they were reaching the drawn set because the only
/// per-poll test was mag3 >= 5.
///
/// 55612 is a person-sized *magnitude*, which is exactly why a magnitude test
/// cannot do this job; the upper bound and the per-axis bound are what reject
/// them, and they are only sound to apply at promotion time.
inline bool promote_plausible(const Vec3f& p, bool is_local_player) {
    if (!shape_ok(p, is_local_player))  return false;
    if (mag3(p) > 3000.f)               return false;   // beyond any real map
    if (std::fabs(p.y) > 200.f)         return false;   // a person is not 200 m up
    return true;
}

BOOL CALLBACK enum_win(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid && GetWindow(h, GW_OWNER) == nullptr && IsWindowVisible(h)) {
        *(HWND*)lp = h;
        return FALSE;
    }
    return TRUE;
}

/// The game window belongs to the game process, so it is found by pid rather
/// than by title or class name -- both of those are the sort of thing that
/// changes between patches, which is exactly what went wrong with the offsets.
HWND game_window() {
    HWND h = nullptr;
    EnumWindows(enum_win, (LPARAM)&h);
    return h;
}

std::wstring window_title(HWND h) {
    wchar_t buf[256] = {};
    GetWindowTextW(h, buf, 256);
    return buf;
}

} // namespace

// ===========================================================================
//  chain
// ===========================================================================

bool Esp::resolve_chain() {
    if (!g_h && !find_process()) return false;

    engine_ = r64(g_base + off::ClientEngine);
    if (!mapped(engine_)) return false;
    gameplay_ = r64(engine_ + off::IGameplay);
    if (!mapped(gameplay_)) return false;
    client_ = r64(gameplay_ + off::ClientPlayer);
    if (!mapped(client_)) return false;
    camera_ = r64(client_ + off::Camera);
    if (!mapped(camera_)) return false;
    local_ = r64(client_ + off::LocalPlayer);
    if (!mapped(local_)) return false;
    actor_instance_ = r64(local_ + off::ActorInst);
    return mapped(actor_instance_);
}

void Esp::trace_chain() const {
    printf("[chain] engine %p gameplay %p client %p camera %p local %p actor %p\n",
           (void*)engine_, (void*)gameplay_, (void*)client_,
           (void*)camera_, (void*)local_, (void*)actor_instance_);
}

// ===========================================================================
//  discovery
// ===========================================================================

void Esp::build_pool(bool merge) {
    // -------------------------------------------------------------------------
    //  merge == true is an INCREMENTAL sweep and it is the fix for the bug that
    //  made the ESP look broken in every match that had already started.
    //
    //  The pool was built once, at the first live position -- which is the
    //  instant a match BEGINS, when the map is nearly empty.  A sweep then
    //  found 1389 plausible candidates and kept 2, because at that moment there
    //  were almost no players to find.  The match then filled up, and the pool
    //  was never rebuilt, so the tracking set could only ever grow out of those
    //  two addresses.  The log said it plainly and I read it as a filter
    //  problem for two runs:
    //
    //      [pool] 2 candidates kept
    //      [live] tracked 2  within 200m 1  on-screen 1  nearest 37.0m
    //
    //  The actor at 37 m was real, and it was one of the two.  Every other
    //  player in the match was invisible to the tool because the sweep had run
    //  before they spawned.
    //
    //  So a rebuild no longer replaces the pool, it EXTENDS it:
    //
    //    * pool_ is kept, so addresses already being watched stay watched
    //    * last_pos_ is kept, so the movement history that promotion depends on
    //      survives, and a candidate that was one movement event short of being
    //      promoted is not reset to zero by a sweep that happens to land
    //    * only genuinely new addresses are appended
    //
    //  This is what makes a periodic rebuild safe to have at all.  The earlier
    //  comment in rescan_if_due describes why periodic rebuilds were disabled:
    //  a sixty-second freeze every thirty seconds.  That objection is about
    //  REPLACING the pool -- which costs a full two-pass sweep and throws away
    //  the movement history -- and it does not apply to extending it, which is
    //  why the two are separate modes rather than one behaviour with a flag.
    //
    //  The cost of a merge sweep is the same two gigabytes of reads.  It is
    //  therefore still not something to do often; it is something safe to do
    //  occasionally, which is what makes the periodic case expressible.
    // -------------------------------------------------------------------------
    if (!merge) {
        pool_.clear();
        last_pos_.clear();
    }

    const size_t pool_before = pool_.size();

    // -----------------------------------------------------------------------
    //  Two passes, and the reason for the second one is a crash.
    //
    //  The obvious implementation collects every mapped pointer into a vector
    //  and then filters it.  That vector reaches 8.4 million entries here --
    //  67 MB -- and with a D3D11 device and swapchain already resident in the
    //  process, the growth of that vector is where an unhandled std::bad_alloc
    //  killed the overlay build without printing a single line.
    //
    //  So the pointer set is never materialised.  Pass one counts the
    //  candidates and records only those that pass the signature: a few
    //  thousand addresses instead of eight million.  Pass two then re-walks the
    //  same regions to re-read each candidate's position, which is one read per
    //  candidate rather than one read per pointer.
    //
    //  The cost is a second sweep, seconds of extra wall clock.  The benefit is
    //  that peak memory is bounded by the candidate count, not by the mapping
    //  count, and the allocator is never asked for 67 MB while a GPU device is
    //  holding memory.
    // -----------------------------------------------------------------------
    constexpr size_t CH = 16u << 20;
    std::vector<uint8_t> buf(CH);
    const uint64_t deadline = GetTickCount64() + 180'000;
    size_t scanned_bytes = 0;

    for (const auto& r : g_regions) {
        if (GetTickCount64() > deadline) break;
        uint64_t lo = (uint64_t)r.BaseAddress, hi = lo + r.RegionSize;
        for (uint64_t a = lo; a < hi && GetTickCount64() < deadline; a += CH) {
            size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
            if (!read(a, buf.data(), want)) continue;
            scanned_bytes += want;
            size_t n = want & ~(size_t)7;
            for (size_t i = 0; i < n; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (!mapped(q)) continue;
                // Only the mapping test here.  An earlier attempt also tested
                // +0x3E0 == 2 inside this loop, to keep the vector small, and it
                // cut the candidate pool from 10,825 to 1,139 -- a 90% loss.
                // The reason is that this field is not stable per object: it is
                // read once per pointer at an arbitrary moment, and the same
                // object reached through a different pointer, or reached a
                // millisecond later, can read 0 or 3.  Filtering on it here
                // threw away candidates that the second pass would have kept.
                //
                // The signature test belongs in the second pass, where it is
                // applied to de-duplicated addresses consistently.
                pool_.push_back(q);
            }
        }
    }

    // -------------------------------------------------------------------------
    //  De-duplicate, and in merge mode preserve what came before.
    //
    //  A sort+unique over the whole pool is correct in both modes -- it is also
    //  what keeps the array ordered, which the poll's round-robin cursor relies
    //  on.  In merge mode the addresses already present survive the unique and
    //  only genuinely new ones are appended, which is the entire point of the
    //  mode: an address that is already being watched must not be reset.
    //
    //  The cursor is re-armed at zero rather than being left where it was.  An
    //  index into a re-sorted array means nothing, and a stale cursor skips
    //  whichever addresses now sit before it -- which in merge mode would be
    //  exactly the new ones at the front, the ones this sweep was run to find.
    // -------------------------------------------------------------------------
    std::sort(pool_.begin(), pool_.end());
    pool_.erase(std::unique(pool_.begin(), pool_.end()), pool_.end());
    printf("[pool] %zu raw candidate addresses from %zu MB scanned%s\n",
           pool_.size(), scanned_bytes >> 20,
           merge ? "  (merge: pool extended, not replaced)" : "");

    // Pass two: apply the signature, then read the position, once per
    // de-duplicated address.  This is where the +0x3E0 test lives -- see the
    // note in the sweep above for why it cannot live there.
    //
    // In merge mode this pass is a filter over old AND new, and the key property
    // is that an address already in the pool is NOT required to re-pass every
    // test to stay.  A single failed read during a sweep -- the object is mid
    // relocation, or the page is briefly unreadable -- would otherwise evict a
    // player who is standing right there.  The pool is extended by a sweep; it
    // is not re-certified by one.
    std::vector<uint64_t> keep;
    keep.reserve(pool_.size());
    size_t passes_field = 0, passes_pos = 0, kept_existing = 0;

    // The addresses that were in the pool before this sweep.  In replace mode
    // this is empty and the loop below is exactly what it always was.
    std::vector<uint64_t> was_present;
    if (merge) {
        was_present.reserve(pool_before);
        // Recover them from last_pos_: it is keyed by address and survives a
        // merge, so it is the authoritative record of what is being watched.
        for (auto& kv : last_pos_) was_present.push_back(kv.first);
        std::sort(was_present.begin(), was_present.end());
    }

    for (uint64_t v : pool_) {
        const bool already_watched =
            merge && std::binary_search(was_present.begin(), was_present.end(), v);

        if (already_watched) {
            // Keep it, without re-testing.  It earned its place by being
            // promoted or by passing a previous sweep; a sweep is not a
            // re-trial, and evicting on a transient read failure is how a
            // tracked player disappears mid-fight.
            ++kept_existing;
            keep.push_back(v);
            continue;
        }

        if (r32(v + off::EntField) != 2) continue;
        ++passes_field;
        Vec3f p{};
        if (!read_pos(v, off::EntPos, p)) continue;
        if (mag3(p) < 5.f) continue;
        ++passes_pos;

        // -------------------------------------------------------------------
        //  THE SKELETON TEST, AND WHY IT BELONGS HERE RATHER THAN AT PROMOTION.
        //
        //  This is the filter that decides "player or scenery", and it is the
        //  one test the pool was missing.  The run of 2026-10-17 in a live match
        //  built a pool of 6,710 candidates and produced zero boxes, and the
        //  cause was visible in the diagnostic line it printed:
        //
        //      [body] nearest 486.2m  pos.y 65.201  root.y -0.000
        //
        //  pos.y was PINNED at 65.201 across every frame of the match while the
        //  distance changed -- a constant is not a position anybody walks with --
        //  and root.y read 0.000.  Two fields of two different objects, 65 m
        //  apart.  On a real actor +0x7C is the feet and +0x208 is the skeleton
        //  root, centimetres apart with the head 2.2 units above; the distance
        //  between them can only be large if the struct is not an actor.
        //
        //  Why here and not at promotion: the earlier note in this file measured
        //  that a +0x3E0-shaped pool in a FIRING RANGE held only 157 objects,
        //  of which two moved.  A live map holds 6,710, because scenery that the
        //  range does not have -- doors, lifts, platforms, debris -- also passes
        //  the shape rules AND moves enough times to satisfy a two-event rule.
        //  Motion alone cannot separate them; it never could, it merely had
        //  nothing to confuse it in the range.  The skeleton can, because a door
        //  has no bones.
        //
        //  Removing them here rather than at promotion also makes the poll
        //  cheaper: the 120 ms cycle walks a few dozen plausible actors instead
        //  of thousands of pieces of rubble, so both the CPU cost and the number
        //  of false motion events drop together.
        //
        //  The local player is exempt: its root is reported on the ground plane,
        //  which can put it outside the root-to-feet tolerance depending on the
        //  animation, and it must stay in the pool because it is what validates
        //  the search.  It is never drawn as an enemy regardless.
        // -------------------------------------------------------------------
        const bool self = is_local(v) || v == local_;
        if (!self) {
            Vec3f root{}, head{};
            if (!read_pos(v, off::BoneRoot, root) || !read_pos(v, off::BoneHead, head)) {
                ++pool_nobone_;
                continue;
            }
            const float up   = head.y - root.y;
            const float span = std::sqrt((head.x - root.x) * (head.x - root.x) +
                                         (head.y - root.y) * (head.y - root.y) +
                                         (head.z - root.z) * (head.z - root.z));
            if (!(up > 0.8f && up < 2.6f && span < 3.f)) {
                ++pool_boneshape_;
                continue;
            }
            const float rdx = root.x - p.x, rdy = root.y - p.y, rdz = root.z - p.z;
            if (std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz) > 1.5f) {
                ++pool_farroot_;
                continue;
            }
        }

        keep.push_back(v);
    }
    pool_.swap(keep);
    printf("[pool]   %zu pass the +0x3E0 signature, %zu also have a world position\n",
           passes_field, passes_pos);
    if (merge)
        printf("[pool]   %zu kept from the previous sweep, %zu new\n",
               kept_existing, pool_.size() - kept_existing);
    printf("[pool]   skeleton filter: %zu rejected with no bone pair, "
           "%zu with a non-human bone shape, %zu with the root away from the feet\n",
           (size_t)pool_nobone_, (size_t)pool_boneshape_, (size_t)pool_farroot_);
    pool_nobone_ = pool_boneshape_ = pool_farroot_ = 0;

    // -----------------------------------------------------------------------
    //  Shape rules.
    //
    //  These are not the test, they are a pre-filter, and the numbers from a
    //  live firing-range frame say why: the field at +0x3E0 reads 2 on 11,703
    //  objects process-wide, and 157 of those also carry a plausible world
    //  position at +0x7C.  Of those 157, two moved in a three-second window --
    //  the local player and the local camera.  The other 155 were scenery whose
    //  +0x7C happens to be a finite float3, and the shape of that noise is
    //  visible in the data:
    //
    //      five objects sharing the prefix 0x...2C49 with y in 0.05..0.09
    //      a long run with two coordinates exactly 0.00
    //      a run of values that are powers-of-two-ish (5.00, 8.00, 16.00, 32.00)
    //
    //  Motion still does the deciding.  These rules only stop the candidate set
    //  from being mostly rubble.
    //
    //  Rule 2 has an exemption, and it is a correction not a convenience: the
    //  local player's own root was measured at y == 0.00 twice, at
    //  (-32.42, 0.00, -12.41) and (-18.42, 0.00, 15.27).  This engine reports the
    //  actor root on the ground plane, not above it.  A rule that deletes the one
    //  object whose presence validates the whole search is not worth having.
    // -----------------------------------------------------------------------
    std::vector<uint64_t> shaped;
    shaped.reserve(pool_.size());
    for (uint64_t v : pool_) {
        // Already-watched addresses skip the shape filter, for the same reason
        // they skip the signature: they are past it.  Re-testing them here also
        // re-introduces a real hazard in merge mode, because scan_plausible
        // rejects a position with y == 0 and this engine reports the local
        // player's own root on the ground plane -- so a sweep that happened to
        // catch the player standing still could evict the one address that
        // validates the whole search.
        const bool already_watched =
            merge && std::binary_search(was_present.begin(), was_present.end(), v);
        if (already_watched) { shaped.push_back(v); continue; }

        Vec3f p{};
        if (!read_pos(v, off::EntPos, p)) continue;
        if (!scan_plausible(p, is_local(v))) { ++rejected_static_; continue; }
        shaped.push_back(v);
    }
    pool_.swap(shaped);

    for (uint64_t v : pool_) {
        if (v == local_) {
            local_in_pool_ = true;
            if (!is_local(v)) local_addrs_.push_back(v);
        }
        // last_pos_ keeps its existing entry when the read fails, so an address
        // that is being watched never loses its movement baseline to a single
        // unreadable moment.
        Vec3f p{};
        if (read_pos(v, off::EntPos, p))
            last_pos_[v] = Vec3{ p.x, p.y, p.z };
    }
    candidate_total_ = pool_.size();
    pool_cursor_ = 0;
    pool_built_ = true;
    last_pool_build_ = GetTickCount();
    printf("[pool] %zu candidates kept (%zu rejected as static shape)\n",
           pool_.size(), (size_t)rejected_static_);
}

// -----------------------------------------------------------------------------
//  The background sweep.
//
//  The rule that governs the whole design: the drawing thread must never wait on
//  the sweep.  So the sweep does not share a pool with the drawing thread while
//  it runs -- it builds its own, untouched by anyone, and takes the lock exactly
//  once at the end for the merge.  Nothing in the loop below holds pool_mtx_,
//  because holding a lock across a sixty-second scan would be the freeze this
//  exists to remove, just spelled differently.
//
//  The cost is that a sweep reads the address space in parallel with the drawing
//  thread's own reads.  That is fine and is not a race: the game's memory is not
//  ours to write, and a value that changes mid-scan (a player moving during the
//  sweep) simply reads as either its old or its new value.  The position test
//  tolerates both -- it only asks for a finite float3 of magnitude > 5 -- and the
//  movement test that promotes an actor runs later, on the drawing thread, over
//  the merged pool.  A sweep has no opinion about motion, so a torn read across
//  it cannot corrupt anything.
// -----------------------------------------------------------------------------

void Esp::request_sweep() {
    if (!sweep_event_) return;
    // Remember that another sweep was wanted, so a request arriving during a run
    // is not silently dropped -- the worker re-arms itself when it finishes.
    InterlockedExchange(&sweep_pending_, 1);
    SetEvent(sweep_event_);
}

bool Esp::sweep_running() const {
    return InterlockedCompareExchange((volatile LONG*)&sweep_active_, 0, 0) != 0;
}

DWORD WINAPI Esp::sweep_thread_entry(LPVOID self) {
    static_cast<Esp*>(self)->sweep_thread_main();
    return 0;
}

void Esp::sweep_thread_main() {
    const HANDLE waits[2] = { sweep_stop_, sweep_event_ };

    for (;;) {
        // Wait for either a request or the stop signal.  WaitAny, not WaitAll:
        // a stop must take effect immediately even with no sweep wanted.
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (w == WAIT_OBJECT_0) break;                 // stop
        if (w != WAIT_OBJECT_0 + 1) break;             // an error -- leave rather than spin
        ResetEvent(sweep_event_);

        InterlockedExchange(&sweep_pending_, 0);
        InterlockedExchange(&sweep_active_, 1);

        // The one thing the worker does NOT do: build into pool_.  build_pool
        // writes pool_, last_pos_, and half a dozen counters; calling it here
        // would mean either locking the drawing thread out for a minute or
        // racing it.  So the sweep is duplicated in a self-contained form that
        // returns its result and touches no member.
        std::vector<uint64_t> found;

        printf("[pool] background sweep started\n");
        fflush(stdout);

        size_t scanned_bytes = 0;
        {
            // Pass one, in behaviour identical to build_pool: walk every mapped
            // region in chunks and record addresses whose pointer value is mapped.
            // The signature test is NOT applied here, for the reason documented in
            // build_pool -- the field is not stable per object and filtering on it
            // during the pointer walk discards 90% of the candidates that the
            // second pass would keep.
            //
            // The difference from build_pool is the destination: a local vector
            // rather than pool_.  That is the whole reason the sweep can run
            // concurrently with the drawing thread.
            std::vector<uint64_t> raw;
            const uint64_t deadline = GetTickCount64() + 180'000;
            std::vector<uint8_t> buf(CH);

            for (const auto& r : g_regions) {
                if (GetTickCount64() > deadline) break;
                const uint64_t lo = (uint64_t)r.BaseAddress;
                const uint64_t hi = lo + r.RegionSize;
                for (uint64_t a = lo; a < hi && GetTickCount64() < deadline; a += CH) {
                    const size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
                    if (!read(a, buf.data(), want)) continue;
                    scanned_bytes += want;
                    const size_t n = want & ~(size_t)7;
                    for (size_t i = 0; i < n; i += 8) {
                        uint64_t q = 0;
                        memcpy(&q, buf.data() + i, 8);
                        if (!mapped(q)) continue;
                        raw.push_back(q);
                    }
                }
            }

            std::sort(raw.begin(), raw.end());
            raw.erase(std::unique(raw.begin(), raw.end()), raw.end());

            printf("[pool] background sweep %zu raw candidates from %zu MB scanned\n",
                   raw.size(), scanned_bytes >> 20);
            fflush(stdout);

            // Pass two: the signature, then the skeleton test, over the
            // de-duplicated set.  This is the same filter the synchronous version
            // applies and it is what decides "skeleton-shaped object" against
            // "scenery that happens to carry these fields".  It runs here rather
            // than at promotion because a candidate that fails it can never be a
            // player, and carrying it through the poll only to discard it costs a
            // read every cycle for nothing.
            size_t passes_field = 0, passes_pos = 0;
            for (uint64_t v : raw) {
                if (r32(v + off::EntField) != 2) continue;
                ++passes_field;

                Vec3f feet{};
                if (!read_pos(v, off::EntPos, feet)) continue;
                if (mag3(feet) < 5.f) continue;
                ++passes_pos;

                Vec3f root{}, head{};
                if (!read_pos(v, off::BoneRoot, root)) continue;
                if (!read_pos(v, off::BoneHead, head)) continue;

                // Bone span: a human is between 0.8 m and 2.6 m from root to
                // head.  Outside that the two fields are not a human skeleton.
                const float up = sqrtf((head.x - root.x) * (head.x - root.x) +
                                       (head.y - root.y) * (head.y - root.y) +
                                       (head.z - root.z) * (head.z - root.z));
                if (!(up > 0.8f && up < 2.6f)) continue;

                // The cross-check that the bone root and the position belong to
                // the same object: both describe where the player is standing, so
                // they must agree to within a generous 1.5 m.  This is the test
                // that separates "wrong animation frame" from "wrong object".
                const float rdx = root.x - feet.x, rdy = root.y - feet.y, rdz = root.z - feet.z;
                const float root_feet = sqrtf(rdx * rdx + rdy * rdy + rdz * rdz);
                if (root_feet > 1.5f) continue;

                found.push_back(v);
            }

            printf("[pool] background sweep: %zu pass the signature, %zu have a "
                   "position, %zu survive the skeleton test\n",
                   passes_field, passes_pos, found.size());
            fflush(stdout);
        }

        // ---- publish ---------------------------------------------------------
        //
        //  The worker does NOT touch pool_ or last_pos_.  Those belong to the
        //  drawing thread, which polls them sixty times a second and would race
        //  a concurrent swap.  Instead the worker drops its findings into an
        //  inbox and forgets about them; poll_pool picks them up at the top of
        //  its next cycle and performs the merge itself.
        //
        //  That inversion is the whole reason this is safe.  The sweep is the
        //  only thing that runs off-thread, and it is also the only thing that
        //  owns no shared state -- it reads the game and posts a list.  Every
        //  structure with an invariant to preserve is touched by exactly one
        //  thread, so there is no invariant to break and no lock to hold across
        //  a scan.
        //
        //  The lock is therefore around a vector push and nothing else, and it
        //  cannot be contended for longer than a memcpy of a few thousand
        //  eight-byte values.
        {
            std::lock_guard<std::mutex> lk(pool_mtx_);
            sweep_inbox_.insert(sweep_inbox_.end(), found.begin(), found.end());
            sweep_inbox_ready_ = true;
            printf("[pool] sweep finished: %zu candidates posted for merge\n",
                   found.size());
            fflush(stdout);
        }

        InterlockedExchange(&sweep_active_, 0);

        // A request that arrived while this sweep ran is honoured now rather than
        // being lost to the ResetEvent above.
        if (InterlockedCompareExchange(&sweep_pending_, 0, 0) != 0)
            SetEvent(sweep_event_);
    }

    InterlockedExchange(&sweep_active_, 0);
}

/// Take whatever the worker posted, merging it into the pool.
///
/// Runs on the drawing thread, from poll_pool, which is the point: pool_ and
/// last_pos_ are only ever written here, so the sweep being concurrent with
/// them is not possible.
///
/// The merge semantics are the ones the synchronous version established:
///
///   * an address already in the pool keeps its place and, critically, its
///     movement history in last_pos_ -- overwriting a baseline here would erase
///     the evidence a promotion is waiting on
///   * only genuinely new addresses are appended, and only they get a baseline
///     seeded
///   * the cursor is re-armed, because pool_ is re-sorted and an index into the
///     old order means nothing
void Esp::merge_sweep_inbox() {
    std::vector<uint64_t> incoming;
    {
        std::lock_guard<std::mutex> lk(pool_mtx_);
        if (!sweep_inbox_ready_) return;
        incoming.swap(sweep_inbox_);
        sweep_inbox_.clear();
        sweep_inbox_ready_ = false;
    }

    if (incoming.empty()) return;

    const size_t before = pool_.size();

    std::vector<uint64_t> merged = pool_;
    merged.insert(merged.end(), incoming.begin(), incoming.end());
    std::sort(merged.begin(), merged.end());
    merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
    pool_.swap(merged);

    const size_t added = pool_.size() - before;

    // Seed a baseline for addresses that are NEW only.  An address already
    // watched keeps whatever baseline it had.
    for (uint64_t v : incoming) {
        if (last_pos_.count(v)) continue;
        Vec3f p{};
        if (read_pos(v, off::EntPos, p))
            last_pos_[v] = Vec3{ p.x, p.y, p.z };
    }

    candidate_total_ = pool_.size();
    pool_cursor_     = 0;
    pool_built_      = true;
    last_pool_build_ = GetTickCount();

    printf("[pool] merge: %zu watched, %zu new, %zu total\n",
           before, added, pool_.size());
}

void Esp::poll_pool() {
    // -----------------------------------------------------------------------
    //  Anything the background sweep posted is merged in here, first, before the
    //  poll reads the pool.
    //
    //  This is the one point in the frame pipeline where pool_ and last_pos_ are
    //  mutated, and it is on the drawing thread, so the two structures have
    //  exactly one writer.  That is what makes the concurrent sweep safe without
    //  a lock around the pool: the sweep never writes them at all.
    // -----------------------------------------------------------------------
    merge_sweep_inbox();

    if (pool_.empty()) return;

    // A quarter of the pool per cycle.  The whole pool is 9,900 four-byte reads,
    // which is affordable once every few frames but not worth doing at 60 Hz,
    // and nothing is lost by spreading it: an actor that moves for a quarter
    // second still ends up tracked.
    size_t slice = pool_.size() / 4;
    if (slice < 256) slice = pool_.size();

    for (size_t i = 0; i < slice; ++i) {
        if (pool_cursor_ >= pool_.size()) pool_cursor_ = 0;
        uint64_t va = pool_[pool_cursor_++];
        ++diag_polls_;

        Vec3f p{};
        if (!read_pos(va, off::EntPos, p)) { ++diag_noread_; continue; }

        // -------------------------------------------------------------------
        //  The full position test, every poll.
        //
        //  This used to be a bare mag3 >= 5, and that is how garbage reached the
        //  promoted set.  A run promoted 0x0000021E11116960 at
        //  (0.00, -9295.86, -142.95), 0x00000030FEEFB250 at (0.00, 14.09, 14.09)
        //  and 0x0000021E47593904 at (55612.56, 0.00, 55608.06) -- nine thousand
        //  metres away, a mirrored template, and fifty-five thousand metres away,
        //  all of them a person-sized object as far as the old check was
        //  concerned.  Note that 0x00000030FEEFB250 is not even a heap address of
        //  the game.
        //
        //  The shape rules were only applied when the pool was built.  The pool
        //  is built once and every address in it is re-read sixty times a second
        //  for the rest of the session; vetting an address at one instant and
        //  then trusting it forever is exactly the mistake that produced those
        //  three lines.
        //
        //  local_ is exempt from the ground rule inside shape_ok().
        // -------------------------------------------------------------------
        if (!promote_plausible(p, is_local(va))) { ++diag_implausible_; continue; }
        float m = mag3(p);
        if (m < 5.f) { ++diag_toosmall_; continue; }

        // Track the local seat as it moves between deaths.  The chain's target
        // changes on every respawn, and this is the only place the pool and the
        // live chain are compared, so it is where the set has to be updated.
        if (va == local_ && !is_local(local_)) local_addrs_.push_back(local_);

        auto it = last_pos_.find(va);
        if (it == last_pos_.end()) { ++diag_first_; last_pos_[va] = { p.x, p.y, p.z }; continue; }

        float dx = p.x - it->second.x, dy = p.y - it->second.y, dz = p.z - it->second.z;
        float moved = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Re-baseline every poll so a moving actor does not keep re-triggering,
        // and a still one does not drift past the threshold through rounding.
        it->second = { p.x, p.y, p.z };

        if (moved < 0.5f) { ++diag_still_; continue; }
        ++diag_moved_;

        // Record the trace before deciding anything, so the unpromoted case is
        // explainable after the fact rather than only measurable while it
        // happens.
        {
            MoverTrace& t = mover_trace_[va];
            if (t.events == 0) t.pos = { p.x, p.y, p.z };
            t.pos = { p.x, p.y, p.z };
            t.travelled += moved;
            ++t.events;
        }

        // A single movement event is a coin flip.  Rejected pools in this session
        // produced 150+ objects that moved once each -- driven by the camera
        // sweeping across the range, not by anything walking.  Promotion needs
        // the same address to move twice, with the second event inside a second
        // and a half of the first, which is what actually separates a player
        // from a value that wobbles when you turn your head.
        auto& hits = mover_hits_[va];
        DWORD  now_hit = GetTickCount();
        if (hits.count == 0 || now_hit - hits.last > 1500) {
            if (hits.count == 0) ++movers_seen_;
            hits.count = 1;
            hits.last  = now_hit;
            ++diag_evt1_;
            continue;
        }
        if (++hits.count < 2) { hits.last = now_hit; ++diag_evt2_; continue; }

        // Anything within two metres of the camera is the camera's own rig --
        // the eye position, the weapon, or the local player's own head.  The
        // firing-range run caught exactly this: a second "mover" at y = 1.51,
        // one metre away, which is eye height, not a person.
        //
        // Two exemptions, both learned from a run where this rejected everything:
        // the local player is never its own enemy, and a camera that reads as
        // the origin is not a camera -- in a dead or spectating state the camera
        // chain returns zeros, and a guard against (0,0,0) then discards every
        // actor within two metres of the map origin, which is a real place.
        if (!is_local(va) && mag3({ cam_pos_.x, cam_pos_.y, cam_pos_.z }) > 1.f) {
            float cdx = p.x - cam_pos_.x, cdy = p.y - cam_pos_.y, cdz = p.z - cam_pos_.z;
            if (std::sqrt(cdx * cdx + cdy * cdy + cdz * cdz) < 2.0f) { ++diag_nearcam_; continue; }
        }

        // -------------------------------------------------------------------
        //  THE BONE CHECK IS NOW A GATE, NOT A DECORATION.
        //
        //  This is the correction for the run that produced 6,710 candidates of
        //  which almost none was a person.  The bone pair was being read and
        //  tested, and then the actor was promoted REGARDLESS of the outcome --
        //  the test only decided whether a.head was set, not whether the object
        //  was an actor at all.  So map geometry that happened to move twice in
        //  a second and a half entered the tracked set, and the log showed what
        //  that looks like: three tracked, zero within 200 m, nearest 486 m.
        //
        //  What the numbers actually said, and why the bone pair is the right
        //  test: on a real actor +0x7C is the feet and +0x208 is the skeleton
        //  root, a few centimetres apart with the head 2.2 units above.  On the
        //  objects that were being promoted, +0x7C read as y = 65.201 -- pinned,
        //  the same value line after line, while the distance changed -- and
        //  +0x208 read as 0.000, sixty-five metres away.  Two unrelated fields
        //  of two unrelated objects.  A feet-to-root distance of 65 m could only
        //  come from a struct that is not an actor, and a struct that does not
        //  have the skeleton cannot be posed, animated, or shot at -- it cannot
        //  be a player whatever else it is.
        //
        //  The thresholds are the ones already measured: 2.2 units root-to-head
        //  on this engine, so 0.8 .. 2.6 is the band and 3.0 the span ceiling.
        //  Both were in the file before; the change is that failing them now
        //  REJECTS rather than merely skipping the head.
        //
        //  Cost: two reads already being made.  A candidate that fails never
        //  reaches actors_, so it is not re-read every frame either -- this
        //  makes the steady state cheaper, not more expensive.
        // -------------------------------------------------------------------
        Vec3f root{}, head{};
        if (!read_pos(va, off::BoneRoot, root) || !read_pos(va, off::BoneHead, head)) {
            ++diag_nobone_;
            continue;
        }
        const float up   = head.y - root.y;
        const float span = std::sqrt((head.x - root.x) * (head.x - root.x) +
                                     (head.y - root.y) * (head.y - root.y) +
                                     (head.z - root.z) * (head.z - root.z));
        if (!(up > 0.8f && up < 2.6f && span < 3.f)) {
            ++diag_boneshape_;
            continue;
        }

        // A human root is within a metre and a half of the feet the position
        // field reports.  This is the cross-check that the two fields belong to
        // the same object: the bone root and +0x7C should agree, because both
        // describe where the player is standing.
        //
        // 1.5 m is generous on purpose -- the root can sit slightly below the
        // feet or slightly above depending on the animation, and a crouch moves
        // it -- while still being far below the 65 m that a mismatched struct
        // produces.  It is the difference between "wrong animation frame" and
        // "wrong object" and it is set to catch only the second.
        const float rdx = root.x - p.x, rdy = root.y - p.y, rdz = root.z - p.z;
        const float root_feet = std::sqrt(rdx * rdx + rdy * rdy + rdz * rdz);
        if (root_feet > 1.5f) {
            ++diag_farroot_;
            continue;
        }

        Actor a;
        a.va        = va;
        a.pos       = { p.x, p.y, p.z };
        a.head      = { head.x, head.y, head.z };
        a.on_screen = true;
        actors_[va] = a;
        local_is_mover_ = local_is_mover_ || is_local(va);
    }
}

void Esp::rescan_if_due() {
    // -----------------------------------------------------------------------
    //  The periodic sweep is now a REQUEST TO THE WORKER, not a call.
    //
    //  The history of this function is the history of one mistake made three
    //  times: doing the sweep on the drawing thread.  At pool_refresh_ms = 30000
    //  the overlay froze for the 40-60 s the sweep took, every thirty seconds.
    //  Setting it to 0 removed the freeze and introduced a worse bug: the pool
    //  was built at the first live position -- the instant a match begins, when
    //  the map is empty -- and never extended, so the log read "1 candidates
    //  kept" and the ESP could not see any player who spawned afterwards.  Both
    //  orders are wrong because the premise is wrong: a two-gigabyte scan does
    //  not belong on the thread that has to present a frame every sixteen
    //  milliseconds, at any interval.
    //
    //  So the sweep moved to its own thread and this function only asks for one.
    //  It returns immediately; the freeze is gone not because the work got
    //  cheaper but because it is no longer on the critical path.  The interval
    //  can therefore be short enough to be useful -- see pool_refresh_ms -- with
    //  no threat to frame pacing.
    //
    //  Sweeps cannot pile up: request_sweep sets an event the worker is waiting
    //  on, and the worker is a single thread that resets it before scanning, so
    //  a request that arrives mid-sweep is remembered as pending and honoured
    //  when that sweep ends rather than starting a second scan in parallel.
    // -----------------------------------------------------------------------
    if (!pool_built_ || cfg_.pool_refresh_ms <= 0)
        return;

    // -----------------------------------------------------------------------
    //  No sweep while there is no live position.
    //
    //  The first background sweep of 2026-10-17 ran twenty seconds after entry,
    //  by which time the player had died and been returned to the menu, and it
    //  reported "0 survive the skeleton test" -- which is the correct answer to
    //  the wrong question.  In a menu there are no players to find, so a sweep
    //  there is two gigabytes of reads and a hundred thousand signature checks
    //  spent to produce nothing, on a thread that is otherwise idle and on a
    //  machine that is otherwise busy rendering a lobby.
    //
    //  waiting_for_match_ is exactly the signal: it is set in the menu, dead,
    //  spectating, or before the match starts, and cleared when a live position
    //  exists.  The one-shot sweep at match entry is unaffected because it does
    //  not go through here.
    // -----------------------------------------------------------------------
    if (waiting_for_match_)
        return;

    if (sweep_running())
        return;   // one at a time; the worker re-arms itself if asked again
    if (GetTickCount() - last_pool_build_ < (DWORD)cfg_.pool_refresh_ms)
        return;

    printf("[pool] scheduling a merge sweep (drawing continues)\n");
    request_sweep();
}

void Esp::validate_pool() {
    // Any address that has ever been the local player counts.  In a deathmatch
    // the seat moves on every death, so the useful question is not "is the
    // current address tracked" -- it is "has any address that was this player
    // been seen moving".  If yes, the rule discriminates and the search works.
    if (actors_.count(local_)) local_is_mover_ = true;
    for (uint64_t a : local_addrs_)
        if (actors_.count(a)) { local_is_mover_ = true; break; }
}

// ===========================================================================
//  observe  --  discovery without drawing
// ===========================================================================

void Esp::observe(int seconds) {
    const DWORD start = GetTickCount();
    const DWORD until = start + (DWORD)seconds * 1000;

    // No pool rebuild during observation.  The rebuild is a real feature of the
    // running ESP -- it picks up new spawns -- but inside a measurement window it
    // does two harmful things: it costs seconds of wall clock, which made a 30 s
    // window report at 61 s, and it replaces the candidate set mid-measurement,
    // discarding the movement history that promotion depends on.  A measurement
    // should measure one thing.
    const int saved_refresh = cfg_.pool_refresh_ms;
    cfg_.pool_refresh_ms = 3600'000;
    printf("[observe] pool rebuild disabled for this window\n");

    // Seed the local-address set before anything is counted, so the verdict's
    // address count reflects the session rather than the first pool match.
    if (local_ && !is_local(local_)) local_addrs_.push_back(local_);

    // ------------------------------------------------------------------------
    //  Reset the rejection counters for this window.
    //
    //  They accumulate otherwise, and the report says "polls this window" --
    //  a window number that is really a session total is worse than no number,
    //  because it cannot be compared with a later run to tell whether a change
    //  helped.
    // ------------------------------------------------------------------------
    diag_polls_ = diag_noread_ = diag_implausible_ = diag_toosmall_ = 0;
    diag_first_ = diag_still_ = diag_moved_ = diag_evt1_ = diag_evt2_ = 0;
    diag_nearcam_ = diag_nobone_ = diag_boneshape_ = diag_farroot_ = 0;

    printf("[observe] watching the pool for %d s -- MOVE, or the movers will not "
           "appear\n", seconds);

    int tick = 0;
    int last_logged_actors = -1;
    std::unordered_set<uint64_t> logged_actors_;
    while (GetTickCount() < until) {
        update_actors();
        // update_actors() respects the 120 ms poll interval; the sleep keeps this
        // loop from spinning the CPU flat while it waits for that interval.
        Sleep(16);

        // -------------------------------------------------------------------
        //  Report promotions as they happen, one line each.
        //
        //  The first version of this printed the whole actor list every time the
        //  count changed.  Since the count changes many times while the pool
        //  warms up, that produced the same address on five consecutive lines and
        //  looked like duplicate tracking -- it was not, it was one actor
        //  reported five times.  This prints only addresses not seen before.
        // -------------------------------------------------------------------
        if ((int)actors_.size() != last_logged_actors) {
            last_logged_actors = (int)actors_.size();
            for (const auto& kv : actors_) {
                const Actor& a = kv.second;
                if (!logged_actors_.insert(a.va).second) continue;
                printf("[promote] 0x%016llX  %8.2f %8.2f %8.2f  dist %6.1f  %s\n",
                       (unsigned long long)a.va, a.pos.x, a.pos.y, a.pos.z,
                       a.distance, is_local(a.va) ? "<- local" : "");
            }
        }

        // Once a second, print where the state is.  This is the point of the
        // whole mode: a line per second that says whether movers are appearing,
        // so an empty result can be told apart from a broken search.
        //
        // fov is included because it is the cheapest in-game indicator there is:
        // 112 means playing, 40 or 60 means menu or death.  A window that runs
        // entirely at 60 produced no data, and without this column that looks
        // identical to a broken search.
        DWORD elapsed = (GetTickCount() - start) / 1000;
        if ((int)elapsed > tick) {
            tick = (int)elapsed;
            printf("[observe] %2ds  tracked %zu   movers %zu   local %-11s fov %.0f\n",
                   tick, actors_.size(), movers_seen_,
                   local_is_mover_ ? "seen moving" : "not yet", cam_fov_);
        }
    }

    cfg_.pool_refresh_ms = saved_refresh;

    // -----------------------------------------------------------------------
    //  Verdict
    //
    //  Written for a deathmatch, where the local player dies and respawns every
    //  few seconds and the chain's target object changes each time.  An earlier
    //  version of this returned early unless the local player was tracked, which
    //  in this mode meant it almost never reached the part that matters.  The
    //  local player is still reported, because the engine tracking its own seat
    //  correctly is meaningful evidence -- but it is evidence, not a gate.
    //
    //  What the verdict is really asking: did the search find objects that move,
    //  are there enough of them to be other players rather than noise, and do
    //  the positions look like places a person can be.
    // -----------------------------------------------------------------------
    printf("\n=== verdict ===\n");
    printf("  candidates in pool    : %zu  (%zu rejected as static shape)\n",
           candidate_total_, rejected_static_);
    printf("  candidates that moved : %zu\n", movers_seen_);
    printf("  local player in pool  : %s  (%zu addresses seen)\n",
           local_in_pool_ ? "yes" : "no", local_addrs_.size());
    printf("  local player tracked  : %s\n", local_is_mover_ ? "yes" : "no");

    // The local player is the control case: you moved, so its address moved.
    // If it is in the pool and still never registered an event, the problem is
    // in the poll loop itself rather than in the promotion rules -- and that is
    // a different fix, so it is worth separating here.
    if (local_) {
        Vec3f lp{};
        if (read_pos(local_, off::EntPos, lp)) {
            auto h = mover_hits_.find(local_);
            auto t = mover_trace_.find(local_);
            printf("  local player control  : pos %.2f %.2f %.2f  events %d  "
                   "hits %d  travelled %.1f\n",
                   lp.x, lp.y, lp.z,
                   t != mover_trace_.end() ? t->second.events : 0,
                   h != mover_hits_.end() ? h->second.count : 0,
                   t != mover_trace_.end() ? t->second.travelled : 0.f);
        }
    }

    // Position plausibility over everything promoted: this is the check that
    // separates "found people" from "found scenery" without needing the local
    // player to be alive at the right instant.
    if (!actors_.empty()) {
        int on_ground = 0, y_zero = 0, axis_pinned = 0;
        float min_y = 1e9f, max_y = -1e9f;
        for (const auto& kv : actors_) {
            const Vec3& p = kv.second.pos;
            if (p.y >= -2.f && p.y <= 4.f) ++on_ground;
            if (p.y == 0.f) ++y_zero;
            if (p.x == 0.f || p.z == 0.f) ++axis_pinned;
            min_y = (p.y < min_y) ? p.y : min_y;
            max_y = (p.y > max_y) ? p.y : max_y;
        }
        printf("\n  promoted-set sanity:\n");
        printf("    y in [-2, 4] (standing height) : %d / %zu\n",
               on_ground, actors_.size());
        printf("    y exactly 0                    : %d\n", y_zero);
        printf("    x or z exactly 0               : %d\n", axis_pinned);
        printf("    y range                        : %.2f .. %.2f\n", min_y, max_y);
    }

    bool ok = !actors_.empty() && actors_.size() >= 2;

    // -----------------------------------------------------------------------
    //  Where every candidate poll ended.
    //
    //  This is the table that turns "0 promoted" from a mystery into a
    //  diagnosis.  A full pool with no promotions means the loop ran a lot and
    //  produced nothing, and the single question is *where* the candidates
    //  stopped.  Read the first column that accounts for the mass:
    //
    //    implausible dominates -> the position test is rejecting the pool
    //    first dominates       -> every address is new on every poll, which
    //                             means last_pos_ is being cleared, i.e. the
    //                             pool is being rebuilt mid-window
    //    still dominates       -> the poll interval is long relative to how far
    //                             these objects move between reads
    //    moved > 0, evt1/evt2  -> movement is detected, the two-event rule is
    //                             what is stopping promotion
    // -----------------------------------------------------------------------
    printf("\n  candidate poll exits (%zu polls this window):\n", diag_polls_);
    printf("    unreadable address            : %zu\n", diag_noread_);
    printf("    implausible position          : %zu\n", diag_implausible_);
    printf("    magnitude < 5                 : %zu\n", diag_toosmall_);
    printf("    first sighting (baselined)    : %zu\n", diag_first_);
    printf("    moved < 0.5 m                 : %zu\n", diag_still_);
    printf("    moved >= 0.5 m  ----------->  : %zu\n", diag_moved_);
    printf("      of those, first event       : %zu\n", diag_evt1_);
    printf("      of those, second event      : %zu\n", diag_evt2_);
    printf("    rejected near camera          : %zu\n", diag_nearcam_);
    printf("    rejected: no bone pair         : %zu\n", diag_nobone_);
    printf("    rejected: bones not a human    : %zu\n", diag_boneshape_);
    printf("    rejected: root far from feet   : %zu\n", diag_farroot_);

    // -----------------------------------------------------------------------
    //  The three bone rejections, explained in place.
    //
    //  They are printed with the same weight as the motion counters because
    //  they are the same kind of number: the reason a candidate did not become
    //  an actor.  The run that added them promoted map geometry, and this is
    //  what would have said so without a game to look at -- diag_farroot_ in
    //  the thousands means the position field and the bone array belong to
    //  different objects, which is a mismatched offset rather than a bad filter.
    // -----------------------------------------------------------------------
    if (diag_farroot_ > 0 || diag_boneshape_ > 0) {
        printf("\n  Bone-based rejections: a candidate that reads as a mover but has\n"
               "  no skeleton at +0x%llX is map geometry -- a door, a lift, a\n"
               "  projectile -- not a player.  A large 'root far from feet' count\n"
               "  would instead mean the two offsets describe different objects.\n",
               (unsigned long long)off::BoneRoot);
    }

    if (actors_.empty()) {
        printf("\n  Nothing was promoted: %zu candidates showed movement, and none\n"
               "  of them twice within the promotion window.  Either the round\n"
               "  ended during the window, or the two-event rule is too strict for\n"
               "  how fast this engine updates positions.\n", movers_seen_);
    } else if (!ok) {
        printf("\n  Only one actor was promoted.  In a deathmatch that is almost\n"
               "  always the local player and nobody else -- other seats exist but\n"
               "  were not moving during the window.  Re-run while a fight is\n"
               "  happening somewhere on the map.\n");
    } else {
        printf("\n  %zu actors promoted out of %zu candidates that moved.  That is\n"
               "  a player set the ESP can draw.  The local player %s among\n"
               "  them, which is %s -- in a deathmatch the seat moves on every\n"
               "  death, so its absence from one window is inconclusive and its\n"
               "  presence is confirmation.\n",
               actors_.size(), movers_seen_,
               local_is_mover_ ? "was" : "was not",
               local_is_mover_ ? "expected and reassuring" : "not a failure on its own");
    }

    // Print the promoted actors sorted by distance, so the nearest ones -- the
    // ones the ESP would draw first -- can be sanity-checked at a glance.
    std::vector<const Actor*> list;
    for (const auto& kv : actors_) list.push_back(&kv.second);
    std::sort(list.begin(), list.end(),
              [](const Actor* a, const Actor* b) { return a->distance < b->distance; });

    if (!list.empty()) {
        // -------------------------------------------------------------------
        //  Print the camera state *here*, not only at the top of the run.
        //
        //  The [check] block prints cam_pos and the basis once, thirty seconds
        //  before this.  A previous attempt to verify the projection by
        //  recomputing it from those numbers produced coordinates that did not
        //  match this table at all, which meant nothing except that the camera
        //  had moved in between.  Any number that explains this table has to be
        //  read at the same moment as this table.
        // -------------------------------------------------------------------
        printf("\n  camera at projection time: pos %.3f %.3f %.3f  fov %.1f  "
               "fwd_sign %d  -> sign %d\n",
               cam_pos_.x, cam_pos_.y, cam_pos_.z, cam_fov_, fwd_sign_,
               fwd_sign_ ? fwd_sign_ : -1);
        printf("  screen %dx%d   live basis row2 (forward axis): "
               "%+.5f %+.5f %+.5f\n", screen_w_, screen_h_,
               view_[6], view_[7], view_[8]);
        printf("  local player: is_local(local_) %s  local_ %p\n",
               local_ ? (is_local(local_) ? "yes" : "no") : "(null)",
               (void*)local_);

        // -------------------------------------------------------------------
        //  Both signs, side by side, for the whole promoted set.
        //
        //  fwd_sign_ has never been established for this build by anything
        //  better than a default: the auto-detection needs the local player at
        //  more than a metre from the eye, and its root sits ten centimetres
        //  away, so the detection has never fired once.  Rather than keep
        //  guessing which sign is right, project every actor under both and
        //  count how many land on screen under each.  The camera is looking at
        //  players, so the correct sign is the one that puts most of them on
        //  screen -- and unlike a single probe, the whole set cannot agree by
        //  accident.
        // -------------------------------------------------------------------
        int on_pos = 0, on_neg = 0;
        for (const Actor* a : list) {
            for (int sg = 1; sg >= -1; sg -= 2) {
                float dx = a->pos.x - cam_pos_.x, dy = a->pos.y - cam_pos_.y,
                      dz = a->pos.z - cam_pos_.z;
                float cx = dx * view_[0] + dy * view_[1] + dz * view_[2];
                float cy = dx * view_[3] + dy * view_[4] + dz * view_[5];
                float cz = dx * view_[6] + dy * view_[7] + dz * view_[8];
                float depth = cz * (float)sg;
                if (depth < 0.05f) continue;
                float aspect = screen_h_ > 0 ? (float)screen_w_ / (float)screen_h_ : 1.777f;
                float fx = (screen_w_ * 0.5f) / std::tan(cam_fov_ * 0.5f * kDeg2Rad);
                float fy = fx / aspect;
                float sx = screen_w_ * 0.5f + cx * fx / depth;
                float sy = screen_h_ * 0.5f - cy * fy / depth;
                bool inside = sx >= 0 && sx <= (float)screen_w_ &&
                              sy >= 0 && sy <= (float)screen_h_;
                if (inside) { if (sg > 0) ++on_pos; else ++on_neg; }
            }
        }
        printf("\n  sign trial over %zu promoted actors:  "
               "fwd_sign=+1 puts %d on screen,  fwd_sign=-1 puts %d on screen\n",
               list.size(), on_pos, on_neg);
        if (on_pos != on_neg) {
            printf("  --> the answer is %+d\n", on_pos > on_neg ? 1 : -1);
        } else {
            printf("  --> inconclusive, both signs agree (%d == %d); the camera\n"
                   "      was not pointed at the promoted set\n", on_pos, on_neg);
        }

        printf("\n  %-20s %-28s %8s  %-14s %s\n",
               "object", "position", "dist", "projects to", "");
        int shown = 0;
        for (const Actor* a : list) {
            if (shown++ >= 25) break;

            // Project every promoted actor, not just one.  A single probe can
            // land just off-screen and look like a failure; the whole set shows
            // whether the projection is consistent, and how many of them land
            // somewhere a player could actually be looking.
            Vec2 s{};
            bool ok = world_to_screen(a->pos, s);
            char proj[32];
            if (ok) snprintf(proj, sizeof proj, "%.0f,%.0f", s.x, s.y);
            else    snprintf(proj, sizeof proj, "behind");

            bool inside = ok && s.x >= 0 && s.x <= (float)screen_w_ &&
                               s.y >= 0 && s.y <= (float)screen_h_;

            // The camera-space coordinates, so the depth sign is checkable
            // without re-deriving it from a stale header.
            float dx = a->pos.x - cam_pos_.x, dy = a->pos.y - cam_pos_.y,
                  dz = a->pos.z - cam_pos_.z;
            float cz = dx * view_[6] + dy * view_[7] + dz * view_[8];
            int   sg = fwd_sign_ ? fwd_sign_ : -1;

            printf("  0x%016llX  %8.2f %8.2f %8.2f  %7.1f  %-14s %-10s "
                   "cz %+8.2f  depth %+8.2f\n",
                   (unsigned long long)a->va, a->pos.x, a->pos.y, a->pos.z,
                   a->distance, proj,
                   inside ? "on screen" : "off screen",
                   cz, cz * (float)sg);
        }
        if (list.size() > 25) printf("  ... %zu more\n", list.size() - 25);

        // The verdict on the projection itself.
        int on_screen = 0, total = 0;
        for (const Actor* a : list) {
            if (a->distance > ui_.esp.max_distance) continue;
            Vec2 s{};
            ++total;
            if (world_to_screen(a->pos, s) &&
                s.x >= 0 && s.x <= (float)screen_w_ &&
                s.y >= 0 && s.y <= (float)screen_h_)
                ++on_screen;
        }
        printf("\n  projection: %d of %d promoted actors land inside the viewport.\n",
               on_screen, total);
        if (total > 0 && on_screen == 0) {
            printf("  NONE of them do, which means the projection is wrong -- the\n"
                   "  forward axis or the field of view.  Boxes would be drawn\n"
                   "  off-screen.  Do not run the overlay until this is fixed.\n");
        } else if (on_screen > 0) {
            printf("  At least one does, so world-to-screen resolves.  Whether the\n"
                   "  boxes land on the right people is what the overlay shows.\n");
        }
    }

    // -----------------------------------------------------------------------
    //  Every candidate that moved, promoted or not.
    //
    //  This is the table that answers the question the verdict line raises.
    //  "11 moved, 0 promoted" is only diagnosable if the 11 can be looked at:
    //  if their positions are all y == 0.00 or all share an axis, the shape
    //  rules are wrong; if they are scattered around the map at plausible
    //  heights, the promotion rule is what needs adjusting; if they cluster at
    //  one point, they are one object being read through several pointers.
    // -----------------------------------------------------------------------
    if (!mover_trace_.empty()) {
        std::vector<std::pair<uint64_t, const MoverTrace*>> tr;
        tr.reserve(mover_trace_.size());
        for (const auto& kv : mover_trace_) tr.emplace_back(kv.first, &kv.second);
        std::sort(tr.begin(), tr.end(), [](const auto& a, const auto& b) {
            return a.second->travelled > b.second->travelled;
        });

        printf("\n  every candidate that moved (by total distance travelled):\n");
        printf("  %-20s %-28s %7s %5s  %s\n",
               "object", "last position", "travelled", "evts", "");
        int n = 0;
        for (const auto& e : tr) {
            if (n++ >= 40) break;
            const MoverTrace& t = *e.second;
            auto h = mover_hits_.find(e.first);
            printf("  0x%016llX  %8.2f %8.2f %8.2f  %7.2f %5d  %s\n",
                   (unsigned long long)e.first, t.pos.x, t.pos.y, t.pos.z,
                   t.travelled, t.events,
                   (h != mover_hits_.end() && h->second.count >= 2)
                       ? "PROMOTABLE" : "single event");
        }
        if (tr.size() > 40) printf("  ... %zu more\n", tr.size() - 40);
    }
}

// ===========================================================================
//  per-frame
// ===========================================================================

void Esp::update_actors() {
    // -----------------------------------------------------------------------
    //  ONE read for camera position, basis and fov -- not three.
    //
    //  These three used to be three separate ReadProcessMemory calls on the same
    //  camera object, ~2 GB/s of game simulation between them.  The game writes
    //  them independently: an ADS transition changes the fov, a scope or a
    //  respawn changes the basis, and a step changes the position.  Reading them
    //  at three different instants means the frame is projected with a basis
    //  from one moment, a position from another and a fov from a third -- and the
    //  visible result is a box that is correct at one distance and off at
    //  another, with no stable error, which is exactly what was reported.
    //
    //  The three fields live in one contiguous span on the camera object:
    //
    //      +0x7C   position   (float3)
    //      +0x41C  basis      (3x3, 12 floats)
    //      +0x454  fov        (float)
    //
    //  so one read of 0x7C .. 0x458 covers all of them and the frame is
    //  internally consistent by construction.  The span is 0x3DC bytes: small,
    //  and one syscall instead of three.
    // -----------------------------------------------------------------------
    constexpr uint64_t kCamBlock   = 0x7C;                  // start
    constexpr size_t   kCamSpan    = 0x458 - 0x7C + 4;      // .. to fov + 4
    static uint8_t cam_buf[kCamSpan];

    bool cam_ok = read(camera_ + kCamBlock, cam_buf, (DWORD)kCamSpan);
    if (cam_ok) {
        const uint8_t* b = cam_buf;
        auto f = [](const uint8_t* p) {
            float v; memcpy(&v, p, 4); return v;
        };
        const size_t o_pos  = 0;                  // +0x7C
        const size_t o_view = 0x41C - 0x7C;       // +0x41C
        const size_t o_fov  = 0x454 - 0x7C;       // +0x454

        Vec3f cp{ f(b + o_pos), f(b + o_pos + 4), f(b + o_pos + 8) };
        cam_pos_ = { cp.x, cp.y, cp.z };
        for (int i = 0; i < 12; ++i) view_[i] = f(b + o_view + i * 4);
        cam_fov_ = f(b + o_fov);
        if (cam_fov_ < 10.f || cam_fov_ > 180.f) cam_fov_ = fov_stable_;
        view_ok_ = true;
    } else {
        view_ok_ = false;
    }

    // Sanity: an all-zero basis is a failed read that still succeeded, which is
    // how a torn read of a relocating object shows up.
    if (view_ok_) {
        float mag = 0.f;
        for (int i = 0; i < 12; ++i) mag += view_[i] * view_[i];
        if (mag < 0.5f) view_ok_ = false;   // an identity basis has mag 3
    }

    // -----------------------------------------------------------------------
    //  Stabilise the fov against killcams and spectating.
    //
    //  Normal gameplay on this build reads 112.  A scope narrows it on purpose
    //  and that is fine -- a genuine zoom should scale the boxes, because the
    //  enemies really are bigger on screen.  What must NOT scale the boxes is
    //  the camera being in a state the player is not aiming from: the killcam on
    //  death and the spectator camera run at 20-31 degrees, which is a tenth of
    //  the gameplay fov, and scaling by it draws every box several times too
    //  large, in the wrong place, or off screen entirely.
    //
    //  The distinction is duration.  A scope is entered and left by the player
    //  and reads its narrow value for as long as they hold it -- but the player
    //  is still alive and still the camera's owner.  A killcam replaces the
    //  camera.  So rather than guess from the value alone, this counts
    //  consecutive frames outside the gameplay band and reports the state; the
    //  drawing path blanks while it holds.  The stable fov is carried forward
    //  from the last good frame either way, so a single odd frame -- a scope
    //  flick, a torn read -- cannot rescale the world for one frame.
    // -----------------------------------------------------------------------
    const bool fov_odd = (cam_fov_ < kFovGameplayLo || cam_fov_ > kFovGameplayHi);
    if (fov_odd) {
        if (fov_odd_frames_ < 1000000) ++fov_odd_frames_;
        cam_fov_ = fov_stable_;          // keep drawing at the last sane scale
    } else {
        fov_stable_ = cam_fov_;
        fov_odd_frames_ = 0;
    }

    // -----------------------------------------------------------------------
    //  The game's client rectangle.
    //
    //  Cached, because the obvious version -- game_window() then GetClientRect,
    //  every frame -- calls EnumWindows, and EnumWindows walks EVERY top-level
    //  window in the system.  At 60 fps that is sixty full walks of the desktop
    //  per second, for a value that changes only when the player alt-tabs into a
    //  different resolution or toggles fullscreen.  It was a measurable share of
    //  the frame budget and bought nothing.
    //
    //  The handle is found once and kept until it stops being a window, which is
    //  the signal that the game restarted.  The rect is re-read every frame,
    //  because that is cheap -- GetClientRect on a handle does not enumerate --
    //  and because a windowed resize must be picked up immediately.
    // -----------------------------------------------------------------------
    HWND w = game_hwnd_;
    if (!w || !IsWindow(w)) {
        w = game_window();
        if (w != game_hwnd_) {
            game_hwnd_ = w;
            printf("[start] game window cached %p\n", (void*)w);
        }
    }
    RECT r{};
    if (w && GetClientRect(w, &r)) {
        screen_w_ = r.right - r.left;
        screen_h_ = r.bottom - r.top;
    }

    const DWORD now = GetTickCount();

    // -----------------------------------------------------------------------
    //  poll_pool() every 120 ms.
    //
    //  The guard used to be pool_cursor_ < pool_.size(), on the theory that the
    //  cursor walks the pool and stops at the end.  It does not: poll_pool()
    //  wraps the cursor back to zero itself, in the middle of its loop, so
    //  after one call the cursor is exactly pool_.size() -- and the guard then
    //  evaluates false forever.  The entire session made one pass over the pool
    //  and stopped.
    //
    //  That is the whole of "0 candidates moved" and "local player events 0":
    //  the pool was read once, at the start, and never again, so nothing could
    //  ever be seen moving.  The exit counters made it visible -- 860 reads over
    //  30 seconds where the arithmetic calls for tens of thousands.
    // -----------------------------------------------------------------------
    if (now - last_poll_ >= 120) {
        last_poll_ = now;
        poll_pool();
    }
    rescan_if_due();
    validate_pool();

    // Refresh the tracked set and drop anything that stopped producing a sane
    // world position, which is what a despawned or pooled actor looks like.
    //
    // The drop threshold is a DURATION, not a frame count.  It used to be
    // "10 consecutive unreadable reads", which is 167 ms at 60 fps and 42 ms at
    // 240 fps -- so the same code held on to a despawned actor four times longer
    // on a slower machine, and gave up on a live one that was merely hard to read
    // four times sooner on a faster one.  A time window is the same on every
    // machine, and it also matches what is actually being tested: how long this
    // address has been unusable, not how many draws have happened since.
    const DWORD now_ms = GetTickCount();
    for (auto it = actors_.begin(); it != actors_.end();) {
        Actor& a = it->second;
        Vec3f p{};
        if (!read_pos(a.va, off::EntPos, p)) {
            if (a.stale_since == 0) a.stale_since = now_ms;
            // 500 ms of silence is a despawn.  A single failed read is normal --
            // the object can be mid-relocation, or the page briefly unreadable --
            // and must not drop a player who is still standing there.
            if (now_ms - a.stale_since > 500) it = actors_.erase(it);
            else ++it;
            continue;
        }
        a.stale_since = 0;
        a.pos = { p.x, p.y, p.z };
        float dx = p.x - cam_pos_.x, dy = p.y - cam_pos_.y, dz = p.z - cam_pos_.z;
        a.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!a.on_screen) {
            Vec3f root{}, head{};
            if (read_pos(a.va, off::BoneRoot, root) && read_pos(a.va, off::BoneHead, head)) {
                float up = head.y - root.y;
                float span = std::sqrt((head.x-root.x)*(head.x-root.x) +
                                       (head.y-root.y)*(head.y-root.y) +
                                       (head.z-root.z)*(head.z-root.z));
                if (up > 0.8f && up < 2.6f && span < 3.f) {
                    a.head = { head.x, head.y, head.z };
                    a.on_screen = true;
                }
            }
        }
        ++it;
    }

    // After the tracked set is current, and before anything projects: decide
    // which direction along row2 is forward.  Done here rather than inside
    // world_to_screen() because that runs once per actor, and a sign that
    // changed partway through a frame would draw half the boxes one way and
    // half the other.
    measure_forward_sign();
}

bool Esp::read_view(float m[12]) const {
    return read(camera_ + off::CamView, m, 48);
}

void Esp::measure_forward_sign() {
    if (fwd_voted_ || actors_.size() < 3) return;
    if (!view_ok_ || screen_w_ <= 0 || screen_h_ <= 0) return;

    // -----------------------------------------------------------------------
    //  THIS IS A DIAGNOSTIC.  IT NO LONGER DECIDES ANYTHING.
    //
    //  The convention was measured directly, by dumping the twelve floats at
    //  +0x41C: the basis is stored COLUMN-major, and the forward axis is the
    //  negative of row2 -- committed as kColMajor / kForwardSign in the header.
    //  That is a fact about this build, not a hypothesis to be re-derived every
    //  run.
    //
    //  The vote that used to live here produced the worst failure mode a
    //  measurement can have: it was INTERMITTENT.  Its score is "how many actors
    //  end up in front", and a point behind the camera, read under the wrong
    //  convention, mirrors to a position that is ALSO on screen with positive
    //  depth -- so the wrong answer scores just as well as the right one
    //  whenever the enemies that happen to be alive are clustered near the
    //  centre.  Which convention it picked therefore depended on where the other
    //  players were standing, and the ESP was correct in one match and wrong in
    //  the next with no code change between them.
    //
    //  It is kept only to report what the vote WOULD have said, so a mismatch
    //  against the committed constant is visible in the log instead of silently
    //  inverting the projection.
    // -----------------------------------------------------------------------
    const float* r0 = view_;
    const float* r1 = view_ + 3;
    const float* r2 = view_ + 6;

    struct Cand { bool col; int sign; int in_front; float sum_sq; };
    Cand best{ false, -1, -1, 0.f };

    for (int col = 0; col <= 1; ++col) {
        for (int sg = 1; sg >= -1; sg -= 2) {
            int   in_front = 0;
            float sum_sq   = 0.f;

            for (const auto& kv : actors_) {
                const Actor& a = kv.second;
                if (is_local(a.va)) continue;

                float dx = a.pos.x - cam_pos_.x, dy = a.pos.y - cam_pos_.y,
                      dz = a.pos.z - cam_pos_.z;

                float cx, cy, cz;
                if (!col) {
                    cx = dx * r0[0] + dy * r0[1] + dz * r0[2];
                    cy = dx * r1[0] + dy * r1[1] + dz * r1[2];
                    cz = dx * r2[0] + dy * r2[1] + dz * r2[2];
                } else {
                    cx = dx * r0[0] + dy * r1[0] + dz * r2[0];
                    cy = dx * r0[1] + dy * r1[1] + dz * r2[1];
                    cz = dx * r0[2] + dy * r1[2] + dz * r2[2];
                }

                float depth = cz * (float)sg;
                if (depth <= 0.05f) continue;

                ++in_front;
                sum_sq += (cx * cx + cy * cy) / (depth * depth);
            }

            if (in_front > best.in_front ||
                (in_front == best.in_front && in_front > 0 && sum_sq < best.sum_sq)) {
                best = { col != 0, sg, in_front, sum_sq };
            }
        }
    }

    const int total = (int)actors_.size() - 1;   // minus the local player
    if (best.in_front <= 0) return;

    // Report, never apply.  col_major_ and fwd_sign_ were set in start() from
    // the measured constants and are not touched here.
    const bool agrees = (best.col == col_major_) && (best.sign == fwd_sign_);
    printf("[fwd] diagnostic vote over %zu actors says %s sign %+d "
           "(in front %d/%d, off-centre %.3f) -- %s committed %s %+d\n",
           actors_.size(), best.col ? "COLUMN" : "ROW", best.sign,
           best.in_front, total, best.sum_sq,
           agrees ? "agrees with" : "DISAGREES WITH",
           col_major_ ? "COLUMN" : "ROW", fwd_sign_);
    fwd_voted_ = true;
}

bool Esp::world_to_screen(const Vec3& world, Vec2& out, float* out_depth) {
    if (!view_ok_ || screen_w_ <= 0 || screen_h_ <= 0) return false;

    // The basis at +0x41C is a true 3x3 rotation: orthonormal to five decimal
    // places.  Its rows are the camera axes in world space, so the camera-space
    // offset is just three dot products.  No projection matrix is needed, and
    // none was ever found -- the earlier "projection matrix" that turned out to
    // be a rotation was this same data misread.
    //
    // -----------------------------------------------------------------------
    //  The rows are three floats apart, not four.
    //
    //  This was read as a padded 4x4 -- r0 at +0, r1 at +4, r2 at +8 -- and
    //  that single stride error is what put every box in the wrong place.  The
    //  [check] print reads the same bytes as a tight 3x3, and under that
    //  reading row1 is the up axis, which a camera's up row always is in a
    //  y-up world:
    //
    //      row1 (stride 3) = +0.00039 +0.99566 +0.09304   <- vertical, correct
    //      row1 (stride 4) = +0.99566 +0.09304 +<next>    <- not vertical
    //
    //  The arithmetic on the second reading even produces unit-length vectors
    //  most of the time, which is why it survived so long: it looks like a
    //  rotation, it just is not this camera's.
    // -----------------------------------------------------------------------
    const float* r0 = view_;
    const float* r1 = view_ + 3;
    const float* r2 = view_ + 6;

    float dx = world.x - cam_pos_.x;
    float dy = world.y - cam_pos_.y;
    float dz = world.z - cam_pos_.z;

    float cx = dx * r0[0] + dy * r0[1] + dz * r0[2];
    float cy = dx * r1[0] + dy * r1[1] + dz * r1[2];
    float cz = dx * r2[0] + dy * r2[1] + dz * r2[2];

    // The forward axis could be row 2 or its negation.  It is decided once, by
    // a vote, in update_actors() -- see measure_forward_sign().  By the time
    // anything projects, fwd_sign_ is set or the measured default applies.
    //
    // -----------------------------------------------------------------------
    //  ROW-MAJOR OR COLUMN-MAJOR -- decided by measurement, not by argument.
    //
    //  The reported symptom is boxes at 180 degrees: the enemy straight ahead is
    //  drawn behind the player.  No sign error can produce that -- a wrong sign
    //  mirrors one axis, while 180 degrees means the forward vector itself is
    //  wrong in the world, which is what happens when a transposed (column-major)
    //  matrix is read row-wise.
    //
    //  The evidence fits it.  row1 = (0, 1, 0) under both readings, because a
    //  vertical axis is vertical either way -- which is why the stride-3 vs
    //  stride-4 test could not distinguish them.  But row0 and row2 come out
    //  perpendicular to each other in the XZ plane, exactly what you get when
    //  the true rows and the true columns are confused.
    //
    //  So the decision is made by the same technique that settled the sign: a
    //  vote over all promoted non-local actors, once per frame, on which reading
    //  puts more of them in FRONT.  The actors the ESP found are people the
    //  player is looking at, so "more of them in front" is not a heuristic -- it
    //  is the only reading consistent with the fact that they were found at all.
    //
    //  cxr/cyr/czr are the row reading; cxc/cyc/czc are the column reading.
    //  Both are needed here because the caller only passes world_to_screen a
    //  position, so the transpose has to be applied at the dot products.
    // -----------------------------------------------------------------------
    float cxc = dx * r0[0] + dy * r1[0] + dz * r2[0];
    float cyc = dx * r0[1] + dy * r1[1] + dz * r2[1];
    float czc = dx * r0[2] + dy * r1[2] + dz * r2[2];

    int sign = fwd_sign_ ? fwd_sign_ : -1;

    // Column reading replaces the row reading when the vote chose it.
    if (col_major_) { cx = cxc; cy = cyc; cz = czc; }

    float depth = cz * (float)sign;
    if (depth < 0.05f) return false;

    // FOV is horizontal in this engine; convert using the real aspect ratio so
    // the vertical scale is not guessed.
    float aspect = screen_h_ > 0 ? (float)screen_w_ / (float)screen_h_ : 1.777f;
    float fx = (screen_w_ * 0.5f) / std::tan(cam_fov_ * 0.5f * kDeg2Rad);
    float fy = fx / aspect;

    out.x = screen_w_ * 0.5f + cx * fx / depth;
    out.y = screen_h_ * 0.5f - cy * fy / depth;
    // Callers that need to scale a world-space size into pixels -- the box
    // height fallback -- need the depth, and recomputing it outside would mean
    // reading the camera twice.
    if (out_depth) *out_depth = depth;
    return true;
}

void Esp::project() {
    for (auto& kv : actors_) {
        Actor& a = kv.second;
        if (is_local(a.va)) continue;                       // never draw yourself
        if (a.distance > ui_.esp.max_distance) continue;

        float depth = 0.f;
        Vec2 root{};
        if (!world_to_screen(a.pos, root, &depth)) {
            // Clear the cached screen position, so a failed projection does not
            // leave last frame's coordinates in place.  Without this, an actor
            // that went behind the camera kept being drawn at wherever it was
            // when it was last visible -- which reads as a box frozen in the
            // wrong place and then jumping.
            a.screen_root = Vec2{};
            a.screen_head = Vec2{};
            a.depth = 0.f;
            continue;
        }
        a.screen_root = root;
        a.depth       = depth;

        Vec2 head{};
        bool have_head = false;
        if (a.on_screen) have_head = world_to_screen(a.head, head);

        if (!have_head) {
            // -----------------------------------------------------------------
            //  Fallback box top, when the bone array is unavailable.
            //
            //  This was wrong in a way that made most boxes unusable.  It read:
            //
            //      head.y = root.y - box_height * 0.5f *
            //               (screen_h / (2 * tan(fov/2)));
            //
            //  screen_root is a pixel coordinate, so that expression multiplies
            //  a world-space height by a pixel focal length and never divides by
            //  the depth.  The result is ~309 pixels at every distance: a box six
            //  hundred pixels tall for a player who is five pixels wide at forty
            //  metres.  Reported as "the boxes are totally wrong".
            //
            //  A world height h at camera depth d covers h * fy / d pixels, where
            //  fy is the vertical focal length.  The depth comes back from
            //  world_to_screen, which is why that function now has an out
            //  parameter for it.
            // -----------------------------------------------------------------
            const float fy = (screen_h_ * 0.5f) /
                             std::tan(cam_fov_ * 0.5f * kDeg2Rad);
            float px = (depth > 0.05f)
                     ? (ui_.esp.box_height * fy / depth)
                     : 0.f;
            // A box is never taller than the screen and never zero.
            if (px > (float)screen_h_) px = (float)screen_h_;
            if (px < 4.f) px = 4.f;

            head.x = root.x;
            head.y = root.y - px;
        }
        a.screen_head = head;
    }
}

// ===========================================================================
//  input
// ===========================================================================

bool Esp::key_once(int vk) {
    static std::unordered_map<int, bool> held;
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool was = held[vk];
    held[vk] = down;
    return down && !was;
}

void Esp::pump_keys() {
    // Every function is reachable from two keys.  The F-row was the original
    // scheme and it works, but plenty of keyboards do not have a usable Insert
    // or have the F-keys claimed by the game or by a laptop's media layer --
    // reported directly by the user.  The number keys are on every keyboard that
    // has a keyboard.
    //
    //   F4  or  1   toggle the ESP on and off
    //   END or  2   open the menu
    //   F1  or  3   boxes
    //   F2  or  4   skeleton
    //   F3  or  5   radar
    //
    // The menu itself is now mouse-driven while it is open, so these toggles are
    // shortcuts that work with the menu closed and with it open -- there is no
    // mode in which a key stops working, which was a source of confusion when
    // the menu had its own keyboard-only interaction.
    if (key_once(VK_F4) || key_once('1')) ui_.esp.enabled   = !ui_.esp.enabled;
    if (key_once(VK_END)|| key_once('2')) ui_.menu.open     = !ui_.menu.open;
    if (key_once(VK_F1) || key_once('3')) ui_.esp.boxes     = !ui_.esp.boxes;
    if (key_once(VK_F2) || key_once('4')) ui_.esp.skeleton  = !ui_.esp.skeleton;
    if (key_once(VK_F3) || key_once('5')) ui_.esp.radar     = !ui_.esp.radar;

    // The menu_open_ mirror is what the rest of the loop reads; keeping the two
    // in step here means nothing else has to know about ui_.
    menu_open_ = ui_.menu.open;

    // ESC twice inside two seconds quits; one ESC is left alone because the
    // game uses it too.
    //
    // Deliberately NOT routed through the menu: the earlier plan was to let ESC
    // close the menu first, and the problem is that the game also uses ESC, so a
    // single press would always do both.  Two presses is unambiguous and is the
    // existing, working quit path.
    static bool armed = false;
    static DWORD at = 0;
    if (key_once(VK_ESCAPE)) {
        if (armed && GetTickCount() - at < 2000) { running_ = false; armed = false; }
        else { armed = true; at = GetTickCount(); }
    } else if (armed && GetTickCount() - at >= 2000) {
        armed = false;
    }
}

// ===========================================================================
//  drawing
// ===========================================================================

void Esp::draw_esp() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    const EspVisual& v = ui_.esp;
    const float scale = dpi_scale();

    // Element colour, resolved once rather than per actor.
    const ImU32 ec = IM_COL32((int)(v.color[0] * 255.f), (int)(v.color[1] * 255.f),
                              (int)(v.color[2] * 255.f), 255);

    // -----------------------------------------------------------------------
    //  Boxes are built from the FEET and a world-space height, never from the
    //  bone array.
    //
    //  The bone-based version was reported as boxes sitting "much too high or
    //  much too low, and sometimes much too small or much too large".  All four
    //  are one cause: the box top came from the bone array at +0x200, and if
    //  that array is misaligned, or a given actor's bones are in a pose the
    //  array does not describe, the top moves to wherever the bad bone read
    //  lands.  The bottom came from the position, so the box spanned from a
    //  correct point to an incorrect one, at whatever size that implied.
    //
    //  A world height projected with the depth is the same statement the game's
    //  own renderer makes: a 1.8 m player at D metres subtends height * fy / D
    //  pixels.  It cannot be "too high" or "too low" -- only as wrong as the
    //  configured height -- and its size tracks distance by construction.
    //  Bones are still read for the skeleton overlay, where their only job is to
    //  show a shape, and where a misaligned bone is visibly a shape problem
    //  rather than a silently wrong box.
    // -----------------------------------------------------------------------
    const float fy = (screen_h_ > 0)
                   ? (screen_h_ * 0.5f) / std::tan(cam_fov_ * 0.5f * kDeg2Rad)
                   : 0.f;
    if (fy <= 0.f) return;

    for (auto& kv : actors_) {
        Actor& a = kv.second;
        if (is_local(a.va) || a.distance > v.max_distance) continue;

        Vec2 r = a.screen_root;
        if (r.x == 0.f && r.y == 0.f) continue;

        // Pixel height of the configured world height at this actor's depth.
        float px = 0.f;
        if (a.depth > 0.05f) {
            px = v.box_height * fy / a.depth;
            if (px > (float)screen_h_) px = (float)screen_h_;
            if (px < 3.f) px = 3.f;
        } else {
            continue;
        }

        // -------------------------------------------------------------------
        //  Smooth toward the fresh value rather than snapping to it.
        //
        //  The read path polls at 120 ms; the draw path runs at 60 fps.  Drawing
        //  the raw sample means the box holds still for seven frames and then
        //  jumps -- the reported "it updates but it stutters".  Following with a
        //  fixed fraction per frame turns those steps into motion.
        //
        //  The factor is frame-rate independent on purpose: a constant 0.35 per
        //  frame would smooth differently at 60 and at 144 fps.  The exponent is
        //  the standard correction, so the box converges in the same wall-clock
        //  time on any monitor.
        //
        //  A teleport -- respawn, a scoped jump across the map -- must NOT be
        //  smoothed, or the box slides across the screen chasing a player who is
        //  suddenly elsewhere.  Anything over 200 px in one frame snaps.
        // -------------------------------------------------------------------
        float alpha = 0.35f;
        if (fps_ > 0 && fps_ < 240) alpha = 1.f - std::pow(1.f - 0.35f, 60.f / (float)fps_);

        if (!a.smooth_valid) {
            a.smooth_root = r;
            a.smooth_h    = px;
            a.smooth_valid = true;
        } else {
            float dx = r.x - a.smooth_root.x, dy = r.y - a.smooth_root.y;
            float jump = std::sqrt(dx * dx + dy * dy);
            if (jump > 200.f) {
                a.smooth_root = r;
                a.smooth_h    = px;
            } else {
                a.smooth_root.x += dx * alpha;
                a.smooth_root.y += dy * alpha;
                a.smooth_h      += (px - a.smooth_h) * alpha;
            }
        }

        r  = a.smooth_root;
        px = a.smooth_h;

        const float top    = r.y - px;
        const float bottom = r.y;
        const float height = bottom - top;
        const float width  = height * 0.45f;

        const ImVec2 tl(r.x - width * 0.5f, top);
        const ImVec2 br(r.x + width * 0.5f, bottom);

        // -------------------------------------------------------------------
        //  Style, per spec §8: a thin, unfilled 2D rectangle in the element
        //  colour.  No fill, no glow, one pixel at the reference scale.
        //
        //  The previous box was a 2 px red rectangle shadowed by a second
        //  black rectangle offset by one pixel -- a deliberate two-pass outline
        //  that read as heavy and coloured.  The spec asks for the opposite:
        //  monochrome, quiet, and legible against a moving game.
        // -------------------------------------------------------------------
        if (v.boxes) {
            dl->AddRect(tl, br, ec, 0.f, 0, 1.f * scale);
        }

        // -------------------------------------------------------------------
        //  Name above the box.  PENDING: the read path has no measured offset
        //  for a player name, so nothing is drawn for it.  The switch exists in
        //  the menu, disabled and labelled, so the gap is visible rather than
        //  silently absent -- and so a name is never invented.
        // -------------------------------------------------------------------

        // -------------------------------------------------------------------
        //  Health bar, left of the box.  3 px wide, 4 px gap, bottom-up.
        //
        //  PENDING: no measured health offset either, so this is not drawn.
        //  The spec is explicit that a real ESP must not use a fictitious
        //  value, and 100% would be a fictitious value.
        // -------------------------------------------------------------------

        if (v.show_distance) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f m", a.distance);
            const float tw = ImGui::GetFont()->CalcTextSizeA(10.f * scale, FLT_MAX,
                                                             0.f, buf).x;
            dl->AddText(ImGui::GetFont(), 10.f * scale,
                        ImVec2(r.x - tw * 0.5f, bottom + 7.f * scale), ec, buf);
        }

        // Skeleton, unchanged in behaviour but moved to its own helper's terms:
        // it is drawn from the bone array, which is what the spec excludes from
        // this stage by default -- so it stays off by default and keeps working
        // for anyone who turns it on.
        if (v.skeleton && a.on_screen) {
            for (int i = 0; i < 5; ++i) {
                Vec3f b0{}, b1{};
                if (!read_pos(a.va, off::BoneRoot + i * 12, b0)) break;
                if (!read_pos(a.va, off::BoneRoot + i * 12 + 12, b1)) break;
                Vec2 s0{}, s1{};
                if (!world_to_screen({ b0.x, b0.y, b0.z }, s0)) continue;
                if (!world_to_screen({ b1.x, b1.y, b1.z }, s1)) continue;
                dl->AddLine(ImVec2(s0.x, s0.y), ImVec2(s1.x, s1.y), ec, 1.f * scale);
            }
        }
    }
}

void Esp::draw_radar() {
    const EspVisual& v = ui_.esp;
    if (!v.radar || screen_w_ <= 0) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float scale = dpi_scale();

    const float size = 160.f * scale;
    const float pad = 24.f * scale;
    ImVec2 c(screen_w_ - size - pad, screen_h_ - size - pad);
    ImVec2 tl(c.x - size * 0.5f, c.y - size * 0.5f);
    ImVec2 br(c.x + size * 0.5f, c.y + size * 0.5f);

    dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 110));
    dl->AddRect(tl, br, IM_COL32(90, 90, 90, 200));

    // Forward is up.  Scale so the radar covers max_distance.
    float scale_r = (size * 0.45f) / (v.max_distance > 1.f ? v.max_distance : 1.f);

    // Under the column reading the forward axis is the third COLUMN, which is
    // view_[2], view_[5], view_[8]; only x and z are needed on the ground plane.
    const float sgn = (float)(fwd_sign_ ? fwd_sign_ : -1);
    float fx = view_[2] * sgn;
    float fz = view_[8] * sgn;
    float fl = std::sqrt(fx * fx + fz * fz);
    if (fl < 1e-4f) return;
    fx /= fl; fz /= fl;
    float rx = -fz, rz = fx;

    // Enemy dots in the element colour, so the radar follows the same palette
    // as everything else rather than being the one red element left over.
    const ImU32 ec = IM_COL32((int)(v.color[0] * 255.f), (int)(v.color[1] * 255.f),
                              (int)(v.color[2] * 255.f), 235);

    for (const auto& kv : actors_) {
        const Actor& a = kv.second;
        if (is_local(a.va) || a.distance > v.max_distance) continue;
        float dx = a.pos.x - cam_pos_.x, dz = a.pos.z - cam_pos_.z;
        float f = dx * fx + dz * fz;
        float rgt = dx * rx + dz * rz;
        ImVec2 p(c.x + rgt * scale_r, c.y - f * scale_r);
        if (p.x < tl.x || p.x > br.x || p.y < tl.y || p.y > br.y) continue;
        dl->AddCircleFilled(p, 3.f * scale, ec);
    }
    // Self marker.
    dl->AddCircleFilled(c, 3.f * scale, ec);
}

void Esp::draw_hud() {
    const EspVisual& v = ui_.esp;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    char buf[256];
    const float scale = dpi_scale();

    // -----------------------------------------------------------------------
    //  The HUD is scaled up with a dark plate behind it.
    //
    //  The default ImGui font is 13 px, which is unreadable over a moving game.
    //  Reported directly: "the HUD up there is too small and hard to see, I
    //  cannot read it".  A font scale plus an opaque backing rectangle makes the
    //  numbers legible at a glance without a menu.
    //
    //  It shows only what is measured: frame rate, counts, the camera
    //  convention.  No invented status.
    // -----------------------------------------------------------------------
    if (v.show_fps) {
        int on_screen = 0;
        for (const auto& kv : actors_) {
            const Actor& a = kv.second;
            if (is_local(a.va) || a.distance > v.max_distance) continue;
            if (a.screen_root.x != 0.f || a.screen_root.y != 0.f) ++on_screen;
        }

        snprintf(buf, sizeof buf,
                 "%d fps   tracked %zu   on-screen %d",
                 fps_, actors_.size(), on_screen);

        const float fs = 13.f * scale;
        const float pad = 8.f * scale;
        const float tw = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, buf).x;
        ImVec2 p(16.f * scale, 16.f * scale);

        dl->AddRectFilled(ImVec2(p.x - pad, p.y - pad),
                          ImVec2(p.x + tw + pad, p.y + fs + pad),
                          IM_COL32(0, 0, 0, 170), 4.f * scale);
        dl->AddText(ImGui::GetFont(), fs, p, IM_COL32(230, 230, 230, 255), buf);

        if (!view_ok_)
            dl->AddText(ImGui::GetFont(), fs,
                        ImVec2(p.x, p.y + fs + pad + 6.f * scale),
                        IM_COL32(220, 140, 140, 255),
                        "view matrix unavailable -- no projection");
    }
}

void Esp::draw_menu(bool* open) {
    // The menu is a module now, not a handful of ImGui widgets.  All this does
    // is keep the two copies of "is the menu open" in step: the class member
    // that draw_frame reads, and the shared UiState the module reads.
    ui_.menu.open = *open;
    bs::draw_menu(ui_);
    *open = ui_.menu.open;
}

void Esp::draw_frame() {
    // No window: nothing to draw into.  This is the --nooverlay mode, and it
    // exists to isolate the overlay window as a suspect for the game's mouse
    // trouble -- so it must not touch the overlay at all, not even begin_frame.
    if (nooverlay_) return;

    // -----------------------------------------------------------------------
    //  Waiting for a match: no ESP, but THE MENU STILL WORKS.
    //
    //  This used to be a bare blank frame and an early return, which made the
    //  menu unreachable in exactly the place it is most wanted.  The tool runs
    //  persistently and sits in the lobby waiting for a live position; the user
    //  opens the menu there to set it up before the match starts.  Returning
    //  early meant pressing the menu key in the lobby did nothing, and did
    //  nothing silently -- no window, no log line, nothing to explain it.
    //
    //  So the frame is still drawn, with the ESP suppressed because there is
    //  nothing valid to project, and the menu is drawn on top because it is the
    //  one thing that needs no game state at all.
    // -----------------------------------------------------------------------
    if (waiting_for_match_) {
        bs::feed_mouse(ui_, ui_.menu.open, overlay_.hwnd());

        overlay_.begin_frame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("##bs_host", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::End();

        draw_menu(&menu_open_);
        overlay_.end_frame();
        return;
    }

    // -----------------------------------------------------------------------
    //  THIS FUNCTION NEVER TOUCHES THE WINDOW.
    //
    //  Three attempts have been made to make the overlay coexist with the game's
    //  input; the third is correct only in contrast to the first two.
    //
    //    (1) A show/hide gate driven by whether the game was the foreground
    //        window.  ShowWindow is an activation event, so showing stole focus,
    //        which triggered a hide, which returned focus, which triggered a
    //        show -- an oscillator that left a black screen up.
    //
    //    (2) Re-focusing the game with AttachThreadInput + SetForegroundWindow
    //        whenever it lost focus.  AttachThreadInput joins this thread to the
    //        foreground thread's input queue -- the same queue the game reads
    //        its mouse buttons from -- and the reported symptom was that the
    //        camera turned but the buttons did nothing, plus the process
    //        hanging.
    //
    //    (3) The gate brought back, hysteretic and by process.  It stopped
    //        oscillating, and it still broke the mouse: it fired twice during a
    //        real match, and the report was the pointer appearing and the game
    //        ignoring clicks.
    //
    //  ShowWindow is not a passive visibility flag.  It maps or unmaps a window
    //  and makes the system re-evaluate activation and the cursor for whatever
    //  owns the foreground.  A game holding the mouse with ClipCursor and raw
    //  input depends on that being stable, so perturbing it mid-match releases
    //  the clip and the game stops seeing buttons.
    //
    //  So this function performs no window operation of any kind.  The window is
    //  shown exactly once, by end_frame on the first presented frame, and never
    //  hidden again.  What "take the overlay off the game" needs is not an
    //  unmapped window but an EMPTY one: the backbuffer is cleared to (0,0,0,0)
    //  every frame, which under DWM per-pixel alpha is fully transparent, so a
    //  frame with nothing drawn on it is invisible over the desktop.
    // -----------------------------------------------------------------------
    bool blank_only = false;

    // -----------------------------------------------------------------------
    //  Blank while the camera is not the player's aiming camera.
    //
    //  On death the engine hands the camera to the killcam, and in spectator to
    //  whatever the spectated player is looking through.  Both run a fov far
    //  outside gameplay (19-31 against a gameplay 112), and both are states the
    //  boxes must not be drawn in: the camera is no longer where the player is
    //  looking, so the projection is correct arithmetic applied to somebody
    //  else's view, and every box is wrong.
    //
    //  This is what "when I die it stops working and then comes back" is.  The
    //  earlier version had no notion of the state at all -- it kept projecting
    //  against the killcam and drew boxes at the wrong size and the wrong place
    //  until respawn restored the camera.  Blanking instead makes the boxes leave
    //  the screen cleanly on death and return on respawn, which is both correct
    //  and what a player expects to see.
    //
    //  Hysteresis via fov_odd_frames_ (set in update_actors) so a single torn fov
    //  read cannot blank the screen for a frame.
    // -----------------------------------------------------------------------
    const bool cam_is_death_or_spectate = (fov_odd_frames_ >= kFovOddFramesToBlank);

    if (HWND tgt = overlay_.target()) {
        // -------------------------------------------------------------------
        //  THE GATE DOES NOT TOUCH THE WINDOW ANY MORE.  This is the fix for
        //  "the cursor appears, and nothing in the game can be clicked".
        //
        //  Every previous version reacted to the game losing the foreground by
        //  calling ShowWindow -- SW_HIDE to take the overlay down, SW_SHOWNA to
        //  bring it back.  Both were reported as breaking the game's input: the
        //  mouse pointer appeared and clicks stopped reaching the game.
        //
        //  The reason is that ShowWindow is not a passive visibility flag.  It
        //  is a window-manager operation: the window is mapped or unmapped, the
        //  activation state is re-evaluated, and the system cursor is re-shown
        //  for whatever window now owns the foreground.  A game that holds the
        //  mouse with ClipCursor and raw input depends on that state being
        //  stable; perturbing it twice during a match releases the clip and the
        //  game stops seeing buttons.  That is exactly the reported symptom, and
        //  it appeared in the log on the very runs where it was described:
        //
        //      [overlay] another application took the foreground -- hidden
        //      [overlay] game is in the foreground again -- shown
        //
        //  So the window is left alone: shown once, at the first presented
        //  frame, and never hidden again for the life of the process.  What
        //  "hiding off the game" actually needs is not an unmapped window -- it
        //  is an EMPTY one.  The backbuffer is cleared to (0,0,0,0) every frame,
        //  which under the DWM per-pixel-alpha composition is fully transparent,
        //  so a frame drawn with no actors on it shows nothing at all over the
        //  desktop.  When the game is not in front this function simply stops
        //  drawing: the overlay goes blank on its own, with no window call
        //  anywhere, and the game's input is never disturbed.
        //
        //  The check is still hysteretic and still by process, because a single
        //  sample of GetForegroundWindow is noise, and because the overlay's own
        //  window can appear as the foreground -- which is not the game losing
        //  focus, it is the tool that draws over the game being on top.
        // -------------------------------------------------------------------
        static DWORD last_check    = 0;
        static bool  game_in_front = true;   // draw until proven otherwise
        static int   disagree      = 0;

        const DWORD now = GetTickCount();

        if (now - last_check >= 250) {
            last_check = now;

            HWND  fg     = GetForegroundWindow();
            DWORD fg_pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &fg_pid);

            const DWORD self_pid = GetCurrentProcessId();
            const bool  own      = (fg_pid == self_pid);
            const bool  observed = own || (fg_pid == g_pid) || (fg_pid == 0);

            if (observed == game_in_front) {
                disagree = 0;
            } else if (++disagree >= 3) {
                game_in_front = observed;
                disagree      = 0;
                printf("[overlay] game is %s the foreground -- %s\n",
                       game_in_front ? "in" : "NOT in",
                       game_in_front ? "drawing" : "blanking (window untouched)");
            }
        }

        // Blank while the game is not in front.
        //
        // NOT a bare return: returning leaves the last frame -- the one with
        // boxes on it -- sitting on screen, because a layered window keeps
        // whatever was presented last.  The fall-through below draws nothing
        // this frame and presents the transparent clear, so the boxes actually
        // leave the screen the moment the game loses the foreground.  The loop
        // then keeps presenting empty frames, which costs nothing measurable and
        // avoids having to track "have I blanked yet".
        blank_only = !game_in_front;
    }

    // Death / spectator blanking joins the foreground gate.  Neither is
    // "the game lost the foreground" but both mean "do not draw", and the effect
    // is identical: an empty transparent frame.
    //
    // This blanks the ESP.  It does NOT blank the menu -- see below, where the
    // menu is drawn unconditionally.  The distinction matters: a menu that
    // vanished whenever the game was not in front, or whenever the player died,
    // would be unusable exactly when it is reached for.
    if (cam_is_death_or_spectate) blank_only = true;

    // The mouse is fed BEFORE begin_frame, and this ordering is load-bearing.
    //
    // begin_frame calls ImGui_ImplWin32_NewFrame and ImGui::NewFrame, and
    // NewFrame latches the whole input queue for the frame about to be built.
    // Events added after it are queued for the frame after, so feeding here
    // instead would put the menu's clicks exactly one frame behind -- which on a
    // drag reads as the slider lagging the cursor and on a click reads as the
    // first press being swallowed.
    //
    // feed_mouse is also what releases the game's ClipCursor on the open
    // transition; that release has to happen before ImGui samples the cursor, or
    // the sampled position is still the clipped one.
    bs::feed_mouse(ui_, ui_.menu.open, overlay_.hwnd());

    overlay_.begin_frame();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##bs_host", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (ui_.esp.enabled && view_ok_ && !blank_only) {
        draw_esp();
        draw_radar();
        draw_hud();

        // -------------------------------------------------------------------
        //  Aim assist, after draw_esp so every actor has a fresh screen
        //  position, and inside this branch so it inherits every gate that
        //  already stops the boxes being drawn.
        //
        //  The gates, restated because they are the whole safety argument:
        //  ui_.esp.enabled (master), view_ok_ (the projection produced sane
        //  output), !blank_only (the game is in the foreground AND the camera is
        //  the player's aiming camera, not a killcam).  The menu being open is a
        //  further gate, because a free pointer over a menu must never be
        //  assisted.  And aim_step itself adds the activation key and the
        //  user-is-moving requirement on top.
        //
        //  Nothing is injected unless all of that holds, which is why the feature
        //  cannot fire in a menu, in a killcam, on the desktop, or while the user
        //  is holding still.
        // -------------------------------------------------------------------
        if (!ui_.menu.open) {
            // Build the candidate list from the already-projected actors.  This
            // is a per-frame gather of at most a few dozen points; it allocates
            // nothing on the heap because the array is fixed and the count is
            // bounded by the tracked-actor cap.
            constexpr int kMaxCandidates = 64;
            AimCandidate cands[kMaxCandidates];
            int n = 0;

            for (const auto& kv : actors_) {
                if (n >= kMaxCandidates) break;
                const Actor& a = kv.second;
                if (is_local(a.va)) continue;
                if (a.distance > ui_.esp.max_distance) continue;

                AimCandidate& c = cands[n++];
                c.x         = a.smooth_valid ? a.smooth_root.x : a.screen_root.x;
                // Aim at the body centre, which is what a box brackets, rather
                // than at the feet.  The feet are where +0x7C is, and aiming
                // there would pull the crosshair to the ground at the target's
                // position -- low by half a body at every distance.
                c.y         = a.smooth_valid ? (a.smooth_root.y - a.smooth_h * 0.5f)
                                             : a.screen_root.y;
                c.distance  = a.distance;
                c.on_screen = (a.screen_root.x != 0.f || a.screen_root.y != 0.f);
            }

            bs::aim_step(ui_.aim, cands, n, screen_w_, screen_h_,
                         /*enabled=*/true);
        } else {
            // Menu open: the pointer is free and over the menu.  Assist off,
            // and the session state dropped so nothing carries across.
            bs::aim_reset();
        }
    } else {
        // ESP disabled, projection invalid, or blanked.  No assist, and no state
        // kept: a match transition must not leave a latched key behind.
        bs::aim_reset();
    }

    // -----------------------------------------------------------------------
    //  One line per second to the log, mirroring what the HUD shows.
    //
    //  The HUD is only visible from the chair, and the whole point of the last
    //  few runs is that the chair and the log disagree: the screen showed
    //  nothing while the log showed a healthy process.  A periodic line makes
    //  the two comparable without a screenshot, and the number that matters is
    //  on_screen -- tracked counts discovery, on_screen counts projection.
    // -----------------------------------------------------------------------
    {
        static DWORD last_report = 0;
        DWORD now = GetTickCount();
        if (now - last_report >= 1000) {
            last_report = now;
            int on_screen = 0, within_range = 0;
            float nearest = 1e9f;
            for (const auto& kv : actors_) {
                const Actor& a = kv.second;
                if (is_local(a.va)) continue;
                if (a.distance < nearest) nearest = a.distance;
                if (a.distance > ui_.esp.max_distance) continue;
                ++within_range;
                if (a.screen_root.x != 0.f || a.screen_root.y != 0.f) ++on_screen;
            }
            printf("[live] fps %d  tracked %zu  within %.0fm %d  on-screen %d  "
                   "nearest %.1fm  reading %s  sign %d  fov %.1f%s\n",
                   fps_, actors_.size(), ui_.esp.max_distance, within_range,
                   on_screen, nearest < 1e9f ? nearest : -1.f,
                   col_major_ ? "COL" : "ROW", fwd_sign_, cam_fov_,
                   cam_is_death_or_spectate ? "  [DEATH/SPECTATE -- blanked]" : "");
            if (overlay_.cpu_present()) {
                printf("[present] cpu path: %.1f ms/frame (%.0f fps ceiling from the "
                       "readback alone)\n",
                       overlay_.last_present_ms(),
                       overlay_.last_present_ms() > 0.0
                           ? 1000.0 / overlay_.last_present_ms() : 0.0);
            }

            // -----------------------------------------------------------------
            //  Where in the body is +0x7C?
            //
            //  This is the measurement that decides the box geometry, and it is
            //  printed rather than reasoned about because the symptom is
            //  ambiguous: a box that looks right up close and drifts upward as
            //  the enemy walks away is consistent with the reference point being
            //  anywhere above the feet, since the error is a fixed fraction of
            //  body height at every distance and only becomes visible when the
            //  box is small.
            //
            //  For each actor the root bone (feet, +0x208) and the head bone
            //  (+0x214) are compared against the +0x7C position in world Y:
            //
            //      pos.y - root.y   how far the reference point sits above the
            //                       feet.  ~0 means +0x7C IS the feet and the
            //                       box should be drawn upward from it; ~0.9
            //                       means it is mid-body and the box must be
            //                       centred on it instead.
            //      head.y - pos.y   how far below the head it sits, which is the
            //                       same question from the top.
            // -----------------------------------------------------------------
            if (!actors_.empty()) {
                float best = 1e9f;
                const Actor* pick = nullptr;
                for (const auto& kv : actors_) {
                    const Actor& a = kv.second;
                    if (is_local(a.va)) continue;
                    if (a.distance < best) { best = a.distance; pick = &a; }
                }
                if (pick) {
                    Vec3f root{}, head{};
                    bool have_root = read_pos(pick->va, off::BoneRoot, root);
                    bool have_head = read_pos(pick->va, off::BoneHead, head);
                    printf("[body] nearest %.1fm  pos.y %.3f  root.y %s%.3f  "
                           "head.y %s%.3f  pos-root %s%.3f  head-pos %s%.3f\n",
                           best,
                           pick->pos.y,
                           have_root ? "" : "?", have_root ? root.y : 0.f,
                           have_head ? "" : "?", have_head ? head.y : 0.f,
                           have_root ? "" : "?", have_root ? pick->pos.y - root.y : 0.f,
                           have_head ? "" : "?", have_head ? head.y - pick->pos.y : 0.f);
                }
            }
        }
    }

    ImGui::End();
    draw_menu(&menu_open_);

    overlay_.end_frame();
}

// ===========================================================================
//  lifecycle
// ===========================================================================

bool Esp::start() {
    printf("[start] locating the game\n");

    // -----------------------------------------------------------------------
    //  The camera basis convention is committed here, from measurement, and is
    //  never re-voted.  See measure_forward_sign().
    // -----------------------------------------------------------------------
    col_major_ = kColMajor;
    fwd_sign_  = kForwardSign;
    printf("[start] camera basis: %s, forward sign %+d (measured, not voted)\n",
           col_major_ ? "COLUMN-major" : "ROW-major", fwd_sign_);

    if (!find_process()) {
        printf("[!] BloodStrike.exe not running or not readable (run elevated)\n");
        return false;
    }
    printf("[start] attached, pid %lu base 0x%llX, %zu regions\n",
           g_pid, (unsigned long long)g_base, g_regions.size());

    if (!resolve_chain()) {
        printf("[!] chain does not resolve yet\n");
        return false;
    }
    trace_chain();

    // Selftest and observe() only read memory and print.  They must not put a
    // window on screen at all.
    //
    // The previous version created the overlay unconditionally, which is what
    // produced a black screen over the whole desktop: a topmost, layered window
    // sized to the game, drawn with no ImGui content yet, click-through so it
    // could not be dismissed, and owned by a process that then sat in a 30 s
    // observation loop.  Nothing in the read path needs a window, so nothing in
    // the read path gets one.
    //
    // This is a flag, not an early return.  Returning here was a bug: it skipped
    // the pool build below, so observe() ran against an empty pool and reported
    // zero candidates and zero rejections -- which reads exactly like "the shape
    // rules are too aggressive" but was in fact "the search never ran".
    const bool headless = selftest_ || nooverlay_;
    if (selftest_)              printf("[start] selftest: no overlay will be created\n");
    else if (nooverlay_)        printf("[start] --nooverlay: reading only, no window at all\n");

    // -----------------------------------------------------------------------
    //  Wait for a live player before doing anything EXPENSIVE.
    //
    //  A dead, spawning, spectating or menu state leaves the local player's
    //  position at (0,0,0) and the chain points at a different object every
    //  second.  Running the scan in that state produces a full, plausible,
    //  completely meaningless report -- "0 candidates promoted" -- which looks
    //  like an algorithm failure and is not one.  players.exe gates on exactly
    //  this (magnitude >= 5), and so does this.
    //
    //  What changes with persist_: this is no longer a gate that can FAIL the
    //  launch.  It is a gate on the pool sweep only.  In persistent mode the
    //  pool sweep is deferred until a player exists -- because sweeping two
    //  gigabytes to find enemies for a match that has not started is wasted work
    //  and is the fifty seconds the user has to sit through -- and everything
    //  else comes up immediately.  The overlay appears at once, draws nothing,
    //  and starts drawing when the match does.
    //
    //  So the thirty-second... sixty-second wait is gone from the launch path
    //  entirely.  It has not been made faster; it has been moved to the moment
    //  it is actually needed, which is when somebody is alive to shoot at.
    // -----------------------------------------------------------------------
    bool live = false;
    if (persist_) {
        printf("[start] persistent mode: overlay comes up now, pool sweeps on "
               "first live position\n");
    } else {
        printf("[start] waiting for a live player (up to 60 s)\n");
        for (int i = 0; i < 60; ++i) {
            if (!resolve_chain()) { Sleep(500); continue; }
            Vec3f lp{};
            if (read_pos(local_, off::EntPos, lp) && mag3(lp) >= 5.f) {
                printf("[start] live: local player at %.2f %.2f %.2f (after %d s)\n",
                       lp.x, lp.y, lp.z, i);
                live = true;
                break;
            }
            if (i % 5 == 0) printf("[start]   %2ds  not spawned yet\n", i);
            Sleep(1000);
        }
        if (!live) {
            printf("[!] the local player never got a world position.  You are in the\n"
                   "    menu, dead, spectating, or the match has not started.  No\n"
                   "    useful measurement is possible in that state -- re-run once\n"
                   "    you are alive and moving.\n");
            return false;
        }
    }

    // -----------------------------------------------------------------------
    //  The background sweep thread comes up here, before anything can ask for a
    //  sweep.  It is started even in headless mode: the pool management is
    //  identical there and a --nooverlay run is exactly the mode in which a long
    //  observation wants sweeps to keep up without stalling the report.
    //
    //  Events, not a condition variable: the worker waits on two handles with
    //  WaitForMultipleObjects so that a stop is honoured immediately even when no
    //  sweep is wanted, which is the property that makes join deterministic.
    //  sweep_event_ is manual-reset and the worker resets it itself before it
    //  starts scanning, so a request that arrives mid-sweep sets the event again
    //  and is seen on the next wait rather than being lost.
    // -----------------------------------------------------------------------
    sweep_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sweep_stop_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (sweep_event_ && sweep_stop_) {
        sweep_thread_ = CreateThread(nullptr, 0, sweep_thread_entry, this, 0, nullptr);
        if (!sweep_thread_) {
            printf("[!] the sweep thread could not be created; sweeps will be "
                   "synchronous and will stall the overlay\n");
        } else {
            printf("[start] background sweep thread up\n");
        }
    } else {
        printf("[!] the sweep events could not be created; sweeps will be "
               "synchronous and will stall the overlay\n");
    }

    if (!headless) {
        HWND w = game_window();
        if (!w) { printf("[!] game window not found\n"); return false; }
        // window_title returns wchar_t, so it cannot go through printf's %s --
        // that was a real invalid read, not a cosmetic warning.  Narrow it.
        std::wstring wt = window_title(w);
        char title_narrow[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, wt.c_str(), -1,
                            title_narrow, (int)sizeof title_narrow - 1,
                            nullptr, nullptr);
        printf("[start] game window %p, title \"%s\"\n", (void*)w, title_narrow);
        // Attach by handle, not by title: this window has no class or title that
        // FindWindowW can be asked about, which is why the first attempt
        // reported "overlay failed" while the window was sitting right there.
        //
        // Present path is chosen before create_on, because it decides the window
        // styles: LAYERED_CPU needs WS_EX_LAYERED and must not take the swapchain
        // flags, and the swapchain path must not be layered.
        overlay_.set_cpu_present(ulw_);
        if (!overlay_.create_on(w)) { printf("[!] overlay failed\n"); return false; }
        printf("[start] overlay up, hwnd %p\n", (void*)overlay_.hwnd());
        stealth::harden_window(overlay_.hwnd());

        // -------------------------------------------------------------------
        //  The UI font, loaded now.
        //
        //  It has to come after create_on, and only after: create_on is what
        //  calls ImGui::CreateContext, and AddFontFromFileTTF on a null context
        //  is an assertion.  Loading is also why the font is not loaded from a
        //  static initialiser -- the ImGui context does not exist until here.
        //
        //  A failure is not fatal.  load_font falls back to ImGui's built-in
        //  font, and the menu then draws in the default face rather than not at
        //  all: a missing Segoe UI would be a cosmetic problem, and a menu that
        //  refuses to open because of a missing font file would be a real one.
        // -------------------------------------------------------------------
        bs::load_font(bs::dpi_scale());
        font_loaded_ = true;
        printf("[start] ui font loaded (segui, dpi scale %.2f)\n", bs::dpi_scale());
    }

    // -----------------------------------------------------------------------
    //  The pool sweep is deferred in persistent mode.
    //
    //  It is the entire fifty-second launch cost: the whole two-gigabyte
    //  readable span of the game is walked looking for candidate addresses, and
    //  it finds enemies.  In a menu there are no enemies, so the sweep is done
    //  and thrown away.  persist_ defers it to the first frame in which the
    //  local player has a position -- that is, to the first frame of a real
    //  match -- and the overlay comes up in the meantime, drawing nothing.
    //
    //  headless (selftest / --nooverlay) has no overlay and no match to wait
    //  for, so it always sweeps immediately.
    // -----------------------------------------------------------------------
    if (persist_ && !headless) {
        printf("[start] pool sweep deferred to first live position -- overlay is up "
               "and will begin drawing when a match starts\n");
        return true;
    }

    printf("[+] scanning for candidates, this takes a while...\n");
    build_pool();

    // -----------------------------------------------------------------------
    //  No projection check here.
    //
    //  There is nothing to check against yet: actors_ is empty until the
    //  observation window below has run, and probing the local player is a
    //  false alarm generator -- its root sits centimetres from the camera, so
    //  its depth is ~0 and the near-plane guard correctly rejects it.  Two runs
    //  were spent reading "BEHIND THE CAMERA" as a projection failure when it
    //  was the projection working.
    //
    //  The check lives at the end of observe(), where there are fourteen actors
    //  to choose a probe from.
    // -----------------------------------------------------------------------
    update_actors();
    printf("[check] screen %dx%d  fov %.1f  fwd_sign %d\n",
           screen_w_, screen_h_, cam_fov_, fwd_sign_);
    printf("[check] cam_pos  %.3f %.3f %.3f\n", cam_pos_.x, cam_pos_.y, cam_pos_.z);
    printf("[check] view basis (3 rows of the 3x3 at +0x41C):\n");
    printf("        row0  %+8.5f %+8.5f %+8.5f\n", view_[0], view_[1], view_[2]);
    printf("        row1  %+8.5f %+8.5f %+8.5f\n", view_[3], view_[4], view_[5]);
    printf("        row2  %+8.5f %+8.5f %+8.5f\n", view_[6], view_[7], view_[8]);
    // -----------------------------------------------------------------------
    //  The raw 48 bytes, all twelve floats, ungrouped.
    //
    //  Whether the rows are three floats apart or four decides the entire
    //  projection, and the two readings are indistinguishable from a grouped
    //  print: a tight 3x3 read as a padded 4x4 shifts every row by one element
    //  and produces three plausible-looking vectors that are not the basis.
    //  Printing the flat array is the only way to tell them apart by eye.  In
    //  the matrix this build returns, row1 read as indices 3,4,5 is the clean
    //  up axis (0, 1, ~0), which is what says the packing is tight.
    // -----------------------------------------------------------------------
    printf("        raw  ");
    for (int i = 0; i < 12; ++i) printf(" %+8.5f", view_[i]);
    printf("\n");
    return true;
}

void Esp::run() {
    running_ = true;
        printf("[+] running -- F4 toggles, END opens the menu\n");

    while (running_) {
        const DWORD now = GetTickCount();

        frames_++;
        if (now - fps_tick_ >= 1000) {
            fps_ = (int)frames_;
            frames_ = 0;
            fps_tick_ = now;
        }

        if (now - last_tick_ >= 16) {
            last_tick_ = now;
            if (!g_h || !resolve_chain()) {
                // The game restarts on map changes and crashes often enough that
                // re-attaching has to be part of the loop, not an error path.
                if (!find_process()) { running_ = false; break; }
                if (!resolve_chain()) continue;
            }

            // ---------------------------------------------------------------
            //  Match detection, and the deferred pool sweep.
            //
            //  In persistent mode nothing was swept at start.  It happens here,
            //  on the first frame in which the local player has a world
            //  position -- the menu -> match transition.  Until then the read
            //  path still runs (the chain is resolved above, update_actors below
            //  keeps waiting_for_match_ current) but nothing is projected and
            //  nothing is drawn, so a stale camera cannot paint garbage over a
            //  menu.
            //
            //  waiting_for_match_ only clears once the position is real.  The
            //  magnitude test is the same one start() used: a menu or a dead
            //  player leaves (0,0,0), which is not a position anybody stands at
            //  -- the smallest maps are tens of metres across.
            // ---------------------------------------------------------------
            if (waiting_for_match_) {
                Vec3f lp{};
                bool have = read_pos(local_, off::EntPos, lp) && mag3(lp) >= 5.f;
                if (have && pool_.empty()) {
                    printf("[match] live position -- sweeping the pool now\n");
                    update_actors();          // refresh camera first, for the sweep
                    build_pool();
                    update_actors();
                }
                if (have) {
                    waiting_for_match_ = false;
                    printf("[match] entered a match at %.2f %.2f %.2f\n",
                           lp.x, lp.y, lp.z);
                } else if (now - wait_report_ms_ >= 10000) {
                    wait_report_ms_ = now;
                    printf("[match] waiting for a live position (menu, dead or "
                           "spectating) -- overlay is up and idle\n");
                }
            } else {
                update_actors();
                project();

                // -----------------------------------------------------------
                //  Leaving a match: go back to waiting.
                //
                //  A deathmatch returns to the menu between rounds and the local
                //  player is despawned.  Without this the flag stays cleared and
                //  the projection runs against a dead camera, painting boxes at
                //  whatever the stale basis produces.  Three consecutive seconds
                //  without a world position is a trip to the menu, not a death --
                //  respawn in these modes is under a second, and a respawn at
                //  (0,0,0) is caught by the magnitude test only while the spawn
                //  animation plays.
                //
                //  Hysteresis on purpose: the single-frame test flickers on
                //  respawn, and flickering here means the pool sweep would be
                //  re-triggered on every death.
                // -----------------------------------------------------------
                Vec3f lp{};
                bool have = read_pos(local_, off::EntPos, lp) && mag3(lp) >= 5.f;
                if (!have) {
                    if (lost_since_ == 0) lost_since_ = now;
                    else if (now - lost_since_ > 3000) {
                        waiting_for_match_ = true;
                        lost_since_ = 0;
                        actors_.clear();
                        printf("[match] back to the menu (no live position for 3 s) "
                               "-- overlay idle, pool kept\n");
                    }
                } else {
                    lost_since_ = 0;
                }
            }
        }

        pump_keys();
        draw_frame();

        // -------------------------------------------------------------------
        //  Panic path.
        //
        //  The overlay is click-through and topmost, so if anything goes wrong
        //  with the foreground logic the user has no way to close it with the
        //  mouse.  Ctrl+Alt+Q is checked on every iteration regardless of what
        //  else is happening, and is deliberately not a key the game uses.
        //  Closing on first press rather than on a double press, because the
        //  whole point is to get out of a stuck state in one keystroke.
        // -------------------------------------------------------------------
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
            (GetAsyncKeyState(VK_MENU)    & 0x8000) &&
            (GetAsyncKeyState('Q')        & 0x8000)) {
            printf("[!] Ctrl+Alt+Q -- shutting down\n");
            running_ = false;
        }

        // If the game has been gone for five seconds, there is nothing left to
        // draw over and no reason to keep a topmost window alive.
        if (!IsWindow(overlay_.target())) {
            if (game_gone_since_ == 0) game_gone_since_ = now;
            else if (now - game_gone_since_ > 5000) {
                printf("[!] the game window is gone -- shutting down\n");
                running_ = false;
            }
        } else {
            game_gone_since_ = 0;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

void Esp::stop() {
    // -----------------------------------------------------------------------
    //  The sweep thread is stopped FIRST, and before g_h is closed.
    //
    //  Order matters here and getting it wrong is a use-after-free, not a
    //  cosmetic problem: the worker reads the game's memory through g_h and
    //  writes into pool_ and last_pos_, which are members of this object.  If the
    //  process handle were closed first, a sweep in flight would be reading a
    //  closed handle; if the Esp were destroyed first, it would be writing to
    //  freed memory.  Both are avoided by the same two lines -- signal the stop,
    //  wait for the thread -- and they must come before anything else in the
    //  teardown.
    //
    //  The wait is bounded rather than INFINITE.  A sweep can legitimately be
    //  ninety seconds into a two-gigabyte walk, and the honest options are to
    //  wait for it or to leave the thread running while the rest of the process
    //  is torn down.  The second is not an option, so the wait is long enough to
    //  cover a real sweep and the thread checks the deadline inside its loops --
    //  a stop requested mid-sweep is seen at the next chunk boundary.
    // -----------------------------------------------------------------------
    if (sweep_stop_) SetEvent(sweep_stop_);
    if (sweep_thread_) {
        const DWORD w = WaitForSingleObject(sweep_thread_, 150'000);
        if (w != WAIT_OBJECT_0)
            printf("[!] the sweep thread did not stop in time\n");
        CloseHandle(sweep_thread_);
        sweep_thread_ = nullptr;
    }
    if (sweep_event_) { CloseHandle(sweep_event_); sweep_event_ = nullptr; }
    if (sweep_stop_)  { CloseHandle(sweep_stop_);  sweep_stop_  = nullptr; }

    // Hide before destroying.  DestroyWindow is enough on a clean exit, but if
    // anything in the teardown throws, an un-hidden topmost window is left on
    // the desktop with nothing left alive to remove it -- a black screen with no
    // owner.  Hiding first means the visible state is already correct before the
    // risky part runs.
    if (overlay_.hwnd() && IsWindow(overlay_.hwnd()))
        ShowWindow(overlay_.hwnd(), SW_HIDE);
    overlay_.destroy();
    if (g_h) { CloseHandle(g_h); g_h = nullptr; }
}

} // namespace bs
} // namespace vex

// ===========================================================================
//  entry point
// ===========================================================================

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    InitializeCriticalSection(&g_log_cs);

    // -----------------------------------------------------------------------
    //  THE LOG FILE IS OPENED FIRST.  THE CONSOLE COMES SECOND.
    //
    //  This order is not cosmetic and was arrived at the hard way: with the
    //  console first, an elevated launch died before writing a single line, and
    //  there was no log at all -- so the failure left no evidence of itself,
    //  which is the one thing a startup path must never do.  AllocConsole and
    //  the freopen of CONOUT$ are the only operations here that can fail on an
    //  elevated process launched with a hidden console, so they are moved after
    //  the file is already open and logging is already possible.
    //
    //  With this order, whatever happens to the console, the file exists from
    //  the first statement and ends at the last thing the process did.
    // -----------------------------------------------------------------------
    {
        char path[MAX_PATH] = {};
        wchar_t wpath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, wpath, MAX_PATH);
        wchar_t* slash = wcsrchr(wpath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        wcscat_s(wpath, L"vex_log.txt");
        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, MAX_PATH, nullptr, nullptr);

        // _fsopen with _SH_DENYWR, not fopen_s.  fopen_s opens for exclusive
        // access, so a running process holds the file against every reader --
        // including the log watcher, and including the operator trying to read
        // the log while the tool is up.  The log is meant to be watched live,
        // so it is opened to deny WRITERS only: this process writes, anything
        // else may read.
        g_log = _fsopen(path, "w", _SH_DENYWR);
        if (!g_log) {
            char tmp[MAX_PATH] = {};
            GetTempPathA(MAX_PATH, tmp);
            strcat_s(tmp, "vex_log.txt");
            g_log = _fsopen(tmp, "w", _SH_DENYWR);
        }
    }

    logf("[boot] pid %lu  log opened (file first, console second)\n",
         GetCurrentProcessId());

    // -----------------------------------------------------------------------
    //  A console, best-effort, and never activated.
    //
    //  A --subsystem:windows binary has no console, so there is normally nowhere
    //  to watch the tool run.  AllocConsole gives it one, and it is deliberately
    //  never brought forward: another window taking the foreground is what made
    //  the game release its clipped mouse in the earliest builds.
    //
    //  Every call here is allowed to fail.  The process must survive a launch
    //  configuration where CONOUT$ cannot be attached, and it must still log --
    //  which it can, because the file is already open above.
    // -----------------------------------------------------------------------
    if (AllocConsole()) {
        FILE* d = nullptr;
        errno_t r = freopen_s(&d, "CONOUT$", "w", stdout);
        g_console_ok = (r == 0) && (d != nullptr);
        SetConsoleTitleW(L"vex -- bs_esp debug");
    }
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    logf("[boot] console %s\n",
         g_console_ok ? "allocated" : "unavailable (file logging only)");
    logf("[start] pid self %lu\n", GetCurrentProcessId());
    vex::stealth::harden_process();
    logf("[start] hardened  (log file written directly, flushed per line)\n");

    wchar_t* cmd = GetCommandLineW();
    bool selftest = cmd && wcsstr(cmd, L"--selftest") != nullptr;
    bool nooverlay = cmd && wcsstr(cmd, L"--nooverlay") != nullptr;
    bool ulw = cmd && wcsstr(cmd, L"--ulw") != nullptr;
    // --once restores the old one-shot behaviour; persistent is the default.
    bool once = cmd && wcsstr(cmd, L"--once") != nullptr;

    {
        vex::bs::Esp esp;
        esp.selftest_  = selftest;
        esp.nooverlay_ = nooverlay;
        esp.ulw_       = ulw;
        esp.persist_   = !once;
        if (!esp.start()) {
            if (!selftest)
                MessageBoxW(nullptr,
                            L"Could not attach to Blood Strike.\n\n"
                            L"Run this elevated: the game process refuses\n"
                            L"OpenProcess without it.",
                            L"vex", MB_ICONERROR | MB_OK);
            return 1;
        }
        if (selftest) {
            // Observe without drawing.  The projection check above is one frame;
            // the question that actually blocks the ESP is whether the pool
            // yields enemies in this game mode, and that needs a window of time
            // -- long enough for someone else on the map to walk somewhere.
            // Twenty seconds is chosen so a stationary player still sees any
            // mover that is already walking when the sample starts.
            // 30 s, not 20: the pool sweep itself takes seconds, and a mode with
            // bots in it still needs somebody to actually walk somewhere before
            // there is anything to promote.  Long enough to see, short enough
            // that a dead round is obvious.
            esp.observe(30);
            esp.stop();
            return 0;
        }
        esp.run();
        esp.stop();
    }
    return 0;
}