// -----------------------------------------------------------------------------
//  bone_find.cpp  --  locate the bone array and its stride by shared translation.
//
//  WHY THIS EXISTS
//
//  The pool filter of 2026-10-17 rejected 1389 candidates down to 2, and the
//  breakdown said why: 920 had "a non-human bone shape" at +0x208/+0x214.  In a
//  live match there are dozens of players; a filter that admits two of them is
//  not a filter, it is a wrong offset.  The pair at +0x208/+0x214 came from an
//  earlier build, and this build has moved it.
//
//  THE FIRST RUN, AND WHAT IT TAUGHT
//
//  The first version scanned every offset at a 4-byte stride and assumed bones
//  were 4 bytes apart -- i.e. a flat float3 run.  It found something better than
//  an answer: six offsets that held two float3s at a CONSTANT human distance for
//  every single one of 120 samples, and one of them at +0x198 with a mean gap of
//  1.000 m.  Constant gap, zero shared motion.
//
//  Zero shared motion is the signature of a pair that is NOT two bones: it is
//  two fields that happen to sit a human distance apart.  And the spacing of
//  those six tells the real story:
//
//      +0x1188   +0x1148      64 bytes apart
//      +0x198    +0x158       64 bytes apart
//
//  A constant 64-byte spacing between "two float3s a human distance apart" is an
//  array of 64-byte structures, each holding two float3s.  Bone data in this
//  engine is packed that way, not as a flat run of float3s.  The scan stride was
//  the bug: at stride 4 the tool can never land on two adjacent bones, so the
//  shared-translation test could not fire even on the correct array.
//
//  So this version scans STRIDE as a parameter, not an assumption, and reports
//  which stride produces a coherent array.  That is the question that was never
//  asked and should have been.
//
//  THE PROPERTY THAT IDENTIFIES A SKELETON
//
//  Unchanged, and still the thing that makes this a measurement rather than a
//  guess: bones move together.  Every bone of a posed skeleton is written in the
//  same frame from the same root transform, so between two samples every bone
//  translates by the same vector -- the player's own movement -- plus a small
//  local component from the animation.  Scenery does not do that.  The correct
//  stride is the one where a run of consecutive slots all move by one vector.
//
//  WHAT IT NEEDS FROM THE OPERATOR
//
//  To be in a match, alive, and MOVING.  The first run moved in 38 of 120
//  frames, which is enough to rank but not enough to be certain, and the tool
//  now says so explicitly in its verdict.
// -----------------------------------------------------------------------------

// WIN32_LEAN_AND_MEAN comes from CMakeLists; repeating it here warns.
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t EntPos       = 0x7C;

    // The chain, as measured by pos_find.  The skeleton is not necessarily on the
    // object that holds the position -- the first run scanned only LocalPlayer,
    // moved properly (118/200 frames) and found nothing at any of five strides,
    // which rules out "wrong stride" and leaves "wrong object".
    constexpr uint64_t ActorInst    = 0x18;
    constexpr uint64_t ActorProps   = 0x88;
    constexpr uint64_t Component    = 0x18;
    constexpr uint64_t IEntity      = 0x40;
}

namespace {

HANDLE   g_h    = nullptr;
uint64_t g_base = 0;
std::vector<MEMORY_BASIC_INFORMATION> g_regions;

inline bool read(uint64_t va, void* d, size_t n) {
    if (!g_h || va == 0) return false;
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_h, (LPCVOID)va, d, n, &got)) return false;
    return got == n;
}
inline uint64_t u64(uint64_t va) { uint64_t v = 0; read(va, &v, 8); return v; }
inline float    f32(uint64_t va) { float    v = 0; read(va, &v, 4); return v; }

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

struct V3 { float x, y, z; };

inline bool read_v3(uint64_t va, V3& v) {
    if (!read(va, &v, 12)) return false;
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 2e4f && std::fabs(v.y) < 2e4f && std::fabs(v.z) < 2e4f;
}
inline bool kzero(const V3& v) {
    return v.x == 0.f && v.y == 0.f && v.z == 0.f;
}
inline float dist3(const V3& a, const V3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// ---------------------------------------------------------------------------
//  Scan parameters.
// ---------------------------------------------------------------------------
constexpr uint64_t kScanFrom = 0x100;
constexpr uint64_t kScanTo   = 0x2400;

/// The strides tried.  This is the whole correction from the first run: the
/// engine packs bone entries as fixed-size records, and the record size is a
/// property of the build, so it is measured rather than assumed.
///
///  12   a flat float3 run -- what the first version assumed and what the
///       +0x208/+0x214 pair implied
///  16   float3 plus a scalar (weight, index, or a pad word)
///  32   float3 plus a transform or a second position
///  48   float3 plus two more
///  64   what the constant-spacing evidence from the first run points at
constexpr size_t kStrides[] = { 12, 16, 32, 48, 64 };
constexpr int    kStrideCount = 5;

constexpr int   kSamples = 200;        // 20 s at 100 ms -- longer than the first
constexpr int   kSleepMs = 100;
constexpr float kMinSharedMove  = 0.03f;
constexpr float kMaxLocalJitter = 0.5f;

/// A skeleton is a RUN of slots, not one pair.  Requiring three in a row is
/// what separates "an array of bones" from "two fields that moved once".
constexpr int kRunLength = 3;

} // namespace

struct SlotStat {
    int   readable   = 0;
    int   shared     = 0;   ///< this slot and the next moved by the same vector
    int   human_gap  = 0;   ///< this slot and the next are a human distance apart
    float sum_gap    = 0.f;
    float sum_move   = 0.f;
};

int main() {
    printf("bone_find -- locate the bone array AND ITS STRIDE\n");
    printf("=================================================\n\n");

    // -- attach -------------------------------------------------------------
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(s, &pe)) {
        do { if (!_wcsicmp(pe.szExeFile, L"BloodStrike.exe")) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    if (!pid) { printf("[X] BloodStrike is not running\n"); return 2; }

    g_h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_h) { printf("[X] OpenProcess failed (%lu) -- run elevated\n", GetLastError()); return 2; }

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(s2, &me)) {
        do { if (!_wcsicmp(me.szModule, L"BloodStrike.exe")) { g_base = (uint64_t)me.modBaseAddr; break; } }
        while (Module32NextW(s2, &me));
    }
    CloseHandle(s2);
    if (!g_base) { printf("[X] module base not found\n"); return 2; }

    {
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        uintptr_t a = 0;
        while ((uint64_t)a < (uint64_t)si.lpMaximumApplicationAddress) {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQueryEx(g_h, (LPCVOID)a, &m, sizeof m) == 0) break;
            if (m.State == MEM_COMMIT && m.RegionSize >= 8 &&
                !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                g_regions.push_back(m);
            uintptr_t nx = (uintptr_t)m.BaseAddress + m.RegionSize;
            if (nx <= a) break;
            a = nx;
        }
        std::sort(g_regions.begin(), g_regions.end(),
                  [](const auto& x, const auto& y) {
                      return (uint64_t)x.BaseAddress < (uint64_t)y.BaseAddress; });
    }
    printf("[*] attached to pid %lu\n", pid);

    // -- local player and the four objects below it -------------------------
    uint64_t engine   = u64(g_base + off::ClientEngine);
    uint64_t gameplay = mapped(engine) ? u64(engine + off::IGameplay) : 0;
    uint64_t client   = mapped(gameplay) ? u64(gameplay + off::ClientPlayer) : 0;
    uint64_t loc      = mapped(client) ? u64(client + off::LocalPlayer) : 0;
    if (!mapped(loc)) { printf("[X] the local chain is broken; be in a match\n"); return 2; }

    // Walk the chain the way pos_find does.  Each hop is optional: a chain that
    // stops at ActorProps is still worth scanning, and refusing to run because
    // the last hop is missing would throw away the objects that DID resolve.
    uint64_t inst  = mapped(loc)   ? u64(loc  + off::ActorInst)  : 0;
    uint64_t props = mapped(inst)  ? u64(inst + off::ActorProps) : 0;
    uint64_t comp  = mapped(props) ? u64(props + off::Component) : 0;
    uint64_t ient  = mapped(comp)  ? u64(comp + off::IEntity)    : 0;

    struct Obj { const char* name; uint64_t va; };
    const Obj objs[] = {
        { "LocalPlayer",   loc   },
        { "ActorInstance", inst  },
        { "ActorProps",    props },
        { "Component",     comp  },
        { "IEntity",       ient  },
    };
    const int kObjCount = 5;

    printf("[*] chain resolved:\n");
    for (int i = 0; i < kObjCount; ++i) {
        printf("      %-13s %p%s\n", objs[i].name, (void*)objs[i].va,
               mapped(objs[i].va) ? "" : "   (unresolved, skipped)");
    }

    V3 pos{};
    if (!read_v3(loc + off::EntPos, pos)) {
        printf("[X] no position at LocalPlayer+0x7C -- not alive, or in a menu\n");
        return 2;
    }
    printf("[*] position %.2f %.2f %.2f\n\n", pos.x, pos.y, pos.z);

    // -------------------------------------------------------------------------
    //  THE SCAN: objects x strides, and the cross product is the measurement.
    //
    //  The first run scanned one object at five strides and found nothing, with
    //  the player moving properly.  That rules out the stride as the only
    //  problem and makes the object the remaining candidate -- so both are
    //  varied at once rather than one being re-tested alone.
    //
    //  A skeleton is a RUN of slots that all move together.  One slot moving
    //  with its neighbour is a coincidence; five in a row is a skeleton.
    // -------------------------------------------------------------------------
    printf("[*] %d objects x %d strides x %d offsets, %d samples (%.0f s)\n",
           kObjCount, kStrideCount, (int)((kScanTo - kScanFrom) / 4), kSamples,
           kSamples * kSleepMs / 1000.0f);
    printf("[!] WALK in a straight line for the whole measurement.\n\n");

    // state[obj][stride][offset]
    struct Prev { V3 a, b; bool ok = false; };
    std::vector<std::unordered_map<uint64_t, Prev>>     prev(kObjCount * kStrideCount);
    std::vector<std::unordered_map<uint64_t, SlotStat>> stat(kObjCount * kStrideCount);

    for (int oi = 0; oi < kObjCount; ++oi) {
        if (!mapped(objs[oi].va)) continue;
        for (int si = 0; si < kStrideCount; ++si) {
            const size_t idx    = (size_t)oi * kStrideCount + si;
            const size_t stride = kStrides[si];
            auto& pm = prev[idx];
            auto& sm = stat[idx];
            for (uint64_t o = kScanFrom; o + stride + 12 <= kScanTo; o += 4) {
                Prev p{};
                p.ok = read_v3(objs[oi].va + o, p.a) &&
                       read_v3(objs[oi].va + o + stride, p.b);
                pm[o] = p;
                sm[o] = SlotStat{};
            }
        }
    }

    int moved_frames = 0;
    const int kPlayerMovedThreshold = 40;

    for (int n = 0; n < kSamples; ++n) {
        V3 now{};
        if (!read_v3(loc + off::EntPos, now)) {
            if (n % 20 == 0) printf("    [%3d/%d] position unreadable\n", n, kSamples);
            Sleep(kSleepMs);
            continue;
        }
        if (dist3(now, pos) > 0.05f) ++moved_frames;
        pos = now;

        for (int oi = 0; oi < kObjCount; ++oi) {
            if (!mapped(objs[oi].va)) continue;
            for (int si = 0; si < kStrideCount; ++si) {
                const size_t idx    = (size_t)oi * kStrideCount + si;
                const size_t stride = kStrides[si];
                auto& pm = prev[idx];
                auto& sm = stat[idx];

                for (auto& kv : sm) {
                    const uint64_t o = kv.first;
                    SlotStat& st = kv.second;
                    Prev& p = pm[o];

                    V3 a{}, b{};
                    const bool ok = read_v3(objs[oi].va + o, a) &&
                                    read_v3(objs[oi].va + o + stride, b);
                    if (!ok || !p.ok) {
                        if (ok) { p.a = a; p.b = b; p.ok = true; }
                        continue;
                    }

                    ++st.readable;

                    const float gap = dist3(a, b);
                    st.sum_gap += gap;
                    if (gap > 0.05f && gap < 1.2f) ++st.human_gap;

                    const float dax = a.x - p.a.x, day = a.y - p.a.y, daz = a.z - p.a.z;
                    const float dbx = b.x - p.b.x, dby = b.y - p.b.y, dbz = b.z - p.b.z;
                    const float mag_a = std::sqrt(dax*dax + day*day + daz*daz);
                    const float mag_b = std::sqrt(dbx*dbx + dby*dby + dbz*dbz);
                    const float ddx = dax - dbx, ddy = day - dby, ddz = daz - dbz;
                    const float jitter = std::sqrt(ddx*ddx + ddy*ddy + ddz*ddz);
                    const float shared = 0.5f * (mag_a + mag_b);
                    st.sum_move += shared;

                    if (shared > kMinSharedMove && jitter < kMaxLocalJitter) ++st.shared;

                    p.a = a; p.b = b;
                }
            }
        }

        if (n % 40 == 0) {
            printf("    [%3d/%d] player moved in %d frames so far\n", n, kSamples, moved_frames);
            fflush(stdout);
        }
        Sleep(kSleepMs);
    }

    printf("\n[*] the player moved in %d of %d sampled frames\n\n", moved_frames, kSamples);

    if (moved_frames < kPlayerMovedThreshold) {
        printf("[!] WARNING: the player barely moved.  The shared-translation test\n"
               "    needs motion, so any result below is unreliable.  Re-run and\n"
               "    keep walking for the whole 20 seconds.\n\n");
    }

    // -------------------------------------------------------------------------
    //  Score every (object, stride) pair and find the best RUN of consecutive
    //  slots in each.  The run is the evidence; a single pair is a coincidence.
    // -------------------------------------------------------------------------
    struct Best {
        int      obj      = -1;
        int      stride_i = -1;
        uint64_t run_off  = 0;
        int      run_len  = 0;
        int      qualifying = 0;
    } best;

    printf("  object        stride  offsets  qualifying  longest run\n");
    printf("  ---------------------------------------------------------\n");

    for (int oi = 0; oi < kObjCount; ++oi) {
        if (!mapped(objs[oi].va)) {
            printf("  %-13s    --   unresolved, skipped\n", objs[oi].name);
            continue;
        }
        for (int si = 0; si < kStrideCount; ++si) {
            const size_t idx    = (size_t)oi * kStrideCount + si;
            const size_t stride = kStrides[si];
            auto& sm = stat[idx];

            std::vector<uint64_t> offs;
            offs.reserve(sm.size());
            for (auto& kv : sm) offs.push_back(kv.first);
            std::sort(offs.begin(), offs.end());

            int run = 0, qualifying = 0;
            uint64_t run_start = 0;

            for (size_t i = 0; i < offs.size(); ++i) {
                const SlotStat& st = sm[offs[i]];
                const bool q = (st.readable  > kSamples / 4)
                            && (st.shared    > kSamples / 8)
                            && (st.human_gap > kSamples / 8);
                if (q) ++qualifying;

                const bool adjacent = (i == 0) || (offs[i] - offs[i - 1] == stride);
                if (q && (run == 0 || adjacent)) {
                    if (run == 0) run_start = offs[i];
                    ++run;
                } else {
                    if (run > best.run_len) {
                        best.run_len  = run;
                        best.run_off  = run_start;
                        best.obj      = oi;
                        best.stride_i = si;
                    }
                    run = q ? 1 : 0;
                    run_start = offs[i];
                }
            }
            if (run > best.run_len) {
                best.run_len  = run;
                best.run_off  = run_start;
                best.obj      = oi;
                best.stride_i = si;
            }
            if (qualifying > best.qualifying) best.qualifying = qualifying;

            printf("  %-13s %5zu  %7zu  %10d  %11d%s\n",
                   objs[oi].name, stride, sm.size(), qualifying, run,
                   (run >= kRunLength) ? "   <-" : "");
        }
    }

    printf("\n");

    // -------------------------------------------------------------------------
    //  Verdict.
    // -------------------------------------------------------------------------
    if (best.obj < 0 || best.run_len < kRunLength) {
        printf("[?] NO CONFIDENT SKELETON.\n");
        printf("    Best was %d consecutive slots", best.run_len);
        if (best.obj >= 0)
            printf(" at %s +0x%llX, stride %zu",
                   objs[best.obj].name, (unsigned long long)best.run_off,
                   kStrides[best.stride_i]);
        printf(", and %d is needed.\n\n", kRunLength);

        printf("    What this run establishes, and what it leaves:\n");
        printf("      - the player moved in %d of %d frames, so motion is not the\n"
               "        problem this time (%d is the bar)\n", moved_frames, kSamples,
               kPlayerMovedThreshold);
        printf("      - %d objects and %d strides were covered, %zu offsets each, so\n"
               "        'wrong stride' and 'wrong object in the chain' are both\n",
               kObjCount, kStrideCount, (size_t)((kScanTo - kScanFrom) / 4));
        printf("        tested, not assumed\n");
        printf("      - what remains is that the bones hang off a further pointer\n"
               "        reached from one of these objects, or sit past +0x%llX\n",
               (unsigned long long)kScanTo);
        printf("\n    Next: follow the pointers out of the object that scored most,\n"
               "    or widen the window.  Both are a change to this tool, not a\n"
               "    guess about the offsets.\n");
        return 1;
    }

    const size_t stride = kStrides[best.stride_i];
    printf("[+] SKELETON FOUND.\n\n");
    printf("    object      : %s  (%p)\n", objs[best.obj].name, (void*)objs[best.obj].va);
    printf("    first bone  : +0x%llX\n", (unsigned long long)best.run_off);
    printf("    stride      : %zu bytes\n", stride);
    printf("    run length  : %d bones\n", best.run_len);
    printf("    covers      : +0x%llX .. +0x%llX\n",
           (unsigned long long)best.run_off,
           (unsigned long long)(best.run_off + stride * (best.run_len - 1)));

    printf("\n    the run, as a skeleton:\n");
    V3 base{};
    if (read_v3(objs[best.obj].va + best.run_off, base)) {
        for (int i = 0; i < best.run_len; ++i) {
            const uint64_t o = best.run_off + stride * (size_t)i;
            V3 v{};
            if (!read_v3(objs[best.obj].va + o, v)) break;
            const float up = v.y - base.y;
            printf("      bone[%2d] +0x%-8llX  %.2f %.2f %.2f   %.2f m above the first%s\n",
                   i, (unsigned long long)o, v.x, v.y, v.z, up,
                   (up > 1.6f && up < 2.5f) ? "   <- head height" : "");
        }
    }

    printf("\n    BoneRoot  = +0x%llX   (on %s)\n",
           (unsigned long long)best.run_off, objs[best.obj].name);
    printf("    BoneHead  = the entry flagged head height above\n");
    printf("    stride    = %zu\n", stride);
    printf("\n    The pool filter and the ESP both assume a 12-byte stride on\n"
           "    LocalPlayer.  Both must be updated to these values, and the filter's\n"
           "    2.2 m head band re-checked against the table above.\n");
    return 0;
}
