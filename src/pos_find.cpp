// -----------------------------------------------------------------------------
//  pos_find.cpp  --  locate the position field by temporal differential.
//
//  Everything except one field is measured and solid.  The chain is stable, the
//  camera tracks the player, EntityMask reads 2, and yet IEntity+0x1C0 was zero
//  for ninety consecutive seconds while the field-of-view changed from 112 to
//  84.1 to 90.3 to 95.1 to 60 -- a player who was very much alive, moving and
//  switching weapons.  One sample had read (-11.43, 0.15, -16.25), two units
//  from the camera, which is what made 0x1C0 look right.
//
//  That is the whole problem with published offsets: one lucky frame is
//  indistinguishable from a correct answer, and it is enough to build five
//  wrong searches on top of.
//
//  So stop trusting the offset and measure the field.  The position has a
//  property nothing else in the object has: it tracks the camera.  Sample the
//  local player's four objects -- IEntity, ActorInstance, ActorProps and
//  Component -- many times over a couple of seconds, and score every 4-byte
//  offset by how often it holds a float3 within a few units of the camera.
//
//  The true offset scores 100%.  Every other offset, including the ones that
//  looked plausible in a single sample, does not.
//
//  This is a local scan over four known objects -- a few hundred kilobytes -- so
//  it runs in milliseconds and can be repeated as often as needed.
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>

namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t Camera       = 0xD88;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t CamPos       = 0x7C;
    constexpr uint64_t CamFov       = 0x454;
    constexpr uint64_t ActorInst    = 0x18;
    constexpr uint64_t ActorProps   = 0x88;
    constexpr uint64_t Component    = 0x18;
    constexpr uint64_t IEntity      = 0x40;
    constexpr uint64_t EntMask      = 0x3E0;
}

namespace {
HANDLE g_h;
uint64_t g_base;
std::vector<MEMORY_BASIC_INFORMATION> g_regions;

inline bool read(uint64_t va, void* d, size_t n) {
    SIZE_T g;
    if (!ReadProcessMemory(g_h, (LPCVOID)va, d, n, &g)) return false;
    return g == n;
}
inline uint64_t u64(uint64_t va) { uint64_t v = 0; read(va, &v, 8); return v; }
inline uint32_t u32(uint64_t va) { uint32_t v = 0; read(va, &v, 4); return v; }
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

constexpr size_t kSpan = 0x3000;          // bytes of each object sampled
constexpr int    kSamples = 300;          // 15 seconds at 50 ms
constexpr int    kSleepMs = 50;
constexpr float  kNearCamera = 5.0f;      // player root sits a couple of metres below the eye

struct Obj { const char* name; uint64_t va; };
} // namespace

int main(int argc, char** argv) {
    printf("pos_find -- locate the position field by temporal differential\n");
    printf("===============================================================\n");

    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(s, &pe)) {
        do { if (!_wcsicmp(pe.szExeFile, L"BloodStrike.exe")) { pid = pe.th32ProcessID; break; } }
        while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    if (!pid) { printf("[X] game not running\n"); return 2; }
    g_h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_h) { printf("[X] OpenProcess failed (%lu)\n", GetLastError()); return 2; }

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(s2, &me)) {
        do { if (!_wcsicmp(me.szModule, L"BloodStrike.exe")) { g_base = (uint64_t)me.modBaseAddr; break; } }
        while (Module32NextW(s2, &me));
    }
    CloseHandle(s2);

    // Enumerate the committed regions so mapped() can answer.  Without this the
    // whole tool reports every hop as broken, which is what the first run did --
    // not a game problem, a missing three-line block.
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
    printf("[+] %zu committed regions\n", g_regions.size());

    // ---- resolve the chain, retrying until every hop maps ----------------
    //
    //  The chain is intermittent: over the runs so far it has resolved cleanly
    //  for 90 consecutive seconds, then failed at LocalPlayer, then resolved
    //  again.  pl+0xDD8 holds a valid pointer most of the time and something
    //  else the rest, which is worth knowing in itself -- a single sample tells
    //  you nothing about whether an offset is right.
    uint64_t engine = 0, gp = 0, pl = 0, cam = 0, loc = 0, inst = 0,
             props = 0, comp = 0, ient = 0;
    int tries = (argc > 1) ? atoi(argv[1]) : 60;
    if (tries < 1) tries = 1;
    for (int t = 0; t < tries; t++) {
        engine = u64(g_base + off::ClientEngine);
        gp = mapped(engine) ? u64(engine + off::IGameplay) : 0;
        pl = mapped(gp) ? u64(gp + off::ClientPlayer) : 0;
        cam = mapped(pl) ? u64(pl + off::Camera) : 0;
        loc = mapped(pl) ? u64(pl + off::LocalPlayer) : 0;
        inst = mapped(loc) ? u64(loc + off::ActorInst) : 0;
        props = mapped(inst) ? u64(inst + off::ActorProps) : 0;
        comp = mapped(props) ? u64(props + off::Component) : 0;
        ient = mapped(comp) ? u64(comp + off::IEntity) : 0;
        // Only the camera is required.  Component+0x40 -> IEntity is intermittent
        // on this build: it resolved for some runs and failed all 60 attempts in
        // others, which makes it useless as a precondition even though it is
        // right most of the time.  Everything that does resolve still gets
        // sampled, and the differential finds the position wherever it lives.
        if (mapped(cam)) break;
        Sleep(500);
    }
    printf("[+] chain: cam %s loc %s inst %s props %s comp %s ient %s\n",
           mapped(cam) ? "ok" : "X", mapped(loc) ? "ok" : "X",
           mapped(inst) ? "ok" : "X", mapped(props) ? "ok" : "X",
           mapped(comp) ? "ok" : "X", mapped(ient) ? "ok" : "X");
    if (!mapped(cam) || !mapped(loc)) { printf("[X] cannot continue\n"); return 3; }

    // Fixed order, and chain[] in the sampler is built to match index for index:
    //   0 LocalPlayer  1 ActorInstance  2 ActorProps  3 Component  4 IEntity
    //   5 Camera -- included as a control.  Its position field is known good, so
    //      it proves the capture mechanism itself sees live data.  If Camera
    //      changes and nothing else does, the other objects are not holding live
    //      state, whatever the chain says.
    std::vector<Obj> objs = {
        { "LocalPlayer",   loc   },
        { "ActorInstance", inst  },
        { "ActorProps",    props },
        { "Component",     comp  },
        { "IEntity",       ient  },
        { "Camera",        cam   },
    };
    printf("[+] sampling %d objects\n", (int)objs.size());
    int waitSeconds = (argc > 2) ? atoi(argv[2]) : 120;
    if (waitSeconds < 1) waitSeconds = 1;
    printf("[+] waiting for the camera to move (up to %d s -- go into a match and move)\n",
           waitSeconds);
    printf("    the capture starts by itself the moment movement is detected\n");
    {
        float prev[3] = { 0,0,0 };
        bool havePrev = false;
        int sustain = 0;                       // consecutive moving samples
        for (int t = 0; t < waitSeconds; t++) {
            uint64_t c2 = u64(g_base + off::ClientEngine);
            uint64_t g2 = mapped(c2) ? u64(c2 + off::IGameplay) : 0;
            uint64_t p2 = mapped(g2) ? u64(g2 + off::ClientPlayer) : 0;
            uint64_t cm2 = mapped(p2) ? u64(p2 + off::Camera) : 0;
            float now[3] = { 0,0,0 };
            if (mapped(cm2)) read(cm2 + off::CamPos, now, 12);
            float cmag = std::sqrt(now[0]*now[0] + now[1]*now[1] + now[2]*now[2]);

            // Two conditions, both necessary.
            //
            // In-world: the lobby and pre-match cameras sit at or near the
            // origin, and "near the camera" then means "near zero", which every
            // uninitialised slot satisfies.  One earlier capture fired on a
            // camera cut between two lobby states and reported 9560 candidate
            // offsets, every one of them a field holding (0,0,0).
            //
            // Sustained: a single moving sample is a camera cut or a spawn
            // animation.  Real movement is consecutive.
            bool inWorld = cmag > 5.f;
            float step = 0;
            if (havePrev) {
                float dx = now[0]-prev[0], dy = now[1]-prev[1], dz = now[2]-prev[2];
                step = std::sqrt(dx*dx + dy*dy + dz*dz);
            }
            if (inWorld && step > 0.02f) sustain++; else sustain = 0;

            if (havePrev && sustain >= 3) {
                printf("    movement detected at %d s (camera at %.2f %.2f %.2f, "
                       "%.0f units out) -- capturing\n\n", t, now[0], now[1], now[2], cmag);
                cam = cm2;
                break;
            }
            memcpy(prev, now, 12);
            havePrev = true;
            if (t % 5 == 0)
                printf("  %3d  camera %.2f %.2f %.2f  %.0f units out  %s\n", t,
                       now[0], now[1], now[2], cmag,
                       inWorld ? "in world" : "AT THE ORIGIN -- not in a match");
            Sleep(1000);
        }
        if (!mapped(cam)) {
            printf("[X] the camera never moved while inside the world.\n");
            printf("    Get into an actual match: this has to be gameplay, not the\n");
            printf("    lobby, not a menu, not a death screen.\n");
            return 4;
        }
    }

    printf("[+] sampling %d objects, %zu bytes each, %d samples every %d ms\n",
           (int)objs.size(), kSpan, kSamples, kSleepMs);
    printf("    keep moving -- this takes %.0f seconds\n\n",
           kSamples * kSleepMs / 1000.0);

    // ---- sample ----------------------------------------------------------
    //
    //  The chain is re-resolved on every single sample.  It used to be resolved
    //  once, before the wait loop, and that produced a capture where the camera
    //  moved 62,139 units in 15 seconds while every float3 on every object held
    //  its value constant -- because those addresses had been freed and
    //  reallocated during the wait, and freed memory does not move.  The
    //  camera was being read live all along, which is exactly why it looked
    //  alive while everything else looked frozen.  Resolving per sample costs
    //  five reads and removes the whole failure mode.
    // Retained per sample, not overwritten -- the earlier version kept one buffer
    // per object, so there was nothing to compare against afterwards and no way
    // to ask which fields had actually changed.
    std::vector<std::vector<std::vector<uint8_t>>> snap(
        objs.size(), std::vector<std::vector<uint8_t>>(kSamples, std::vector<uint8_t>(kSpan)));
    std::vector<std::vector<float>>    cams(kSamples, std::vector<float>(3, 0.f));
    std::vector<std::vector<uint64_t>> addrs(kSamples, std::vector<uint64_t>(objs.size(), 0));
    int chainOk = 0;
    int moved = 0;                                // samples where an address changed

    for (int i = 0; i < kSamples; i++) {
        // rebuild the full chain from the global every time
        uint64_t e2 = u64(g_base + off::ClientEngine);
        uint64_t g2 = mapped(e2) ? u64(e2 + off::IGameplay) : 0;
        uint64_t q2 = mapped(g2) ? u64(g2 + off::ClientPlayer) : 0;
        uint64_t c2 = mapped(q2) ? u64(q2 + off::Camera) : 0;
        // chain[] must line up with objs[]; see the construction below
        uint64_t chain[6] = { 0, 0, 0, 0, 0, 0 };
        chain[0] = mapped(q2) ? u64(q2 + off::LocalPlayer) : 0;   // LocalPlayer
        chain[1] = mapped(chain[0]) ? u64(chain[0] + off::ActorInst) : 0;  // ActorInstance
        chain[2] = mapped(chain[1]) ? u64(chain[1] + off::ActorProps) : 0; // ActorProps
        chain[3] = mapped(chain[2]) ? u64(chain[2] + off::Component) : 0;  // Component
        chain[4] = mapped(chain[3]) ? u64(chain[3] + off::IEntity) : 0;    // IEntity
        chain[5] = c2;                                              // Camera

        // LocalPlayer through Component must hold; IEntity is intermittent on
        // this build and is optional rather than fatal.
        bool ok = mapped(c2);
        for (int k = 0; k < 4; k++) if (!mapped(chain[k])) ok = false;
        if (ok) chainOk++;

        if (mapped(c2)) read(c2 + off::CamPos, cams[i].data(), 12);
        for (size_t o = 0; o < objs.size(); o++) {
            uint64_t va = chain[o];
            addrs[i][o] = va;
            if (i && addrs[i-1][o] && va != addrs[i-1][o]) moved++;
            if (!mapped(va)) { memset(snap[o][i].data(), 0, kSpan); continue; }
            read(va, snap[o][i].data(), kSpan);
        }
        Sleep(kSleepMs);
    }
    printf("[+] chain resolved on %d/%d samples; object address changed on %d\n",
           chainOk, kSamples, moved);
    if (chainOk < kSamples * 9 / 10) {
        printf("[X] the chain did not hold for the whole capture -- the results\n");
        printf("    below would be sampling a mix of live and stale memory.\n");
        return 3;
    }

    // did the camera actually move?  if it did not, every score is meaningless
    float camTravel = 0;
    for (int i = 1; i < kSamples; i++) {
        float dx = cams[i][0]-cams[i-1][0], dy = cams[i][1]-cams[i-1][1],
              dz = cams[i][2]-cams[i-1][2];
        camTravel += std::sqrt(dx*dx + dy*dy + dz*dz);
    }
    printf("[+] camera moved %.2f units over the window, from %.2f %.2f %.2f to "
           "%.2f %.2f %.2f\n", camTravel,
           cams.front()[0], cams.front()[1], cams.front()[2],
           cams.back()[0], cams.back()[1], cams.back()[2]);
    if (camTravel < 0.5f) {
        printf("[X] the camera did not move -- move during the sampling window.\n");
        return 4;
    }

    // ---- score every offset ----------------------------------------------
    struct Hit { size_t obj; uint32_t off; int nearC; float travel; float sample[3]; };
    std::vector<Hit> hits;

    for (size_t o = 0; o < objs.size(); o++) {
        for (uint32_t off = 0; off + 12 <= kSpan; off += 4) {
            int nearC = 0, finiteCount = 0;
            float first[3] = { 0,0,0 }, last[3] = { 0,0,0 };
            float travel = 0;
            bool haveFirst = false;
            for (int i = 0; i < kSamples; i++) {
                float p[3];
                memcpy(p, &snap[o][i][off], 12);
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                    continue;
                if (std::fabs(p[0]) > 1e5f || std::fabs(p[1]) > 1e5f || std::fabs(p[2]) > 1e5f)
                    continue;
                finiteCount++;
                float d = std::sqrt((p[0]-cams[i][0])*(p[0]-cams[i][0]) +
                                    (p[1]-cams[i][1])*(p[1]-cams[i][1]) +
                                    (p[2]-cams[i][2])*(p[2]-cams[i][2]));
                if (d <= kNearCamera &&
                    std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]) >= 5.f) {
                    nearC++;
                    if (!haveFirst) { memcpy(first, p, 12); haveFirst = true; }
                    memcpy(last, p, 12);
                }
            }
            if (nearC < 2) continue;
            // among the near samples, how far did the value move?
            if (haveFirst) {
                float dx = last[0]-first[0], dy = last[1]-first[1], dz = last[2]-first[2];
                travel = std::sqrt(dx*dx + dy*dy + dz*dz);
            }
            hits.push_back({ o, off, nearC, travel, { last[0], last[1], last[2] } });
        }
    }

    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.nearC != b.nearC) return a.nearC > b.nearC;
        return a.travel > b.travel; });

    printf("\n%-16s %-7s %-7s %-9s %s\n", "object", "offset", "near", "travel", "last sample");
    int shown = 0;
    for (auto& h : hits) {
        if (shown++ >= 25) { printf("    ... %zu more\n", hits.size() - 25); break; }
        // Recompute travel here from the retained snapshots rather than trusting
        // the value accumulated during scoring -- it came out as 0.00 for every
        // row, which is obviously wrong given the camera travelled 14.82 units,
        // and a column of zeroes is worse than no column at all: it reads as
        // "these fields are stale" when in fact the measurement was broken.
        float first[3] = { 0,0,0 }, last[3] = { 0,0,0 };
        bool have = false;
        for (int i = 0; i < kSamples; i++) {
            float p[3];
            memcpy(p, &snap[h.obj][i][h.off], 12);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
            float d = std::sqrt((p[0]-cams[i][0])*(p[0]-cams[i][0]) +
                                (p[1]-cams[i][1])*(p[1]-cams[i][1]) +
                                (p[2]-cams[i][2])*(p[2]-cams[i][2]));
            if (d > kNearCamera) continue;
            // and it must be a place in the world, not a default slot
            if (std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]) < 5.f) continue;
            if (!have) { memcpy(first, p, 12); have = true; }
            memcpy(last, p, 12);
        }
        float dx = last[0]-first[0], dy = last[1]-first[1], dz = last[2]-first[2];
        float travel = have ? std::sqrt(dx*dx + dy*dy + dz*dz) : 0.f;
        printf("%-16s +0x%04X  %3d/%d  %8.2f  %9.2f %9.2f %9.2f%s\n",
               objs[h.obj].name, h.off, h.nearC, kSamples, travel,
               last[0], last[1], last[2],
               h.nearC >= kSamples * 9 / 10
                   ? "   <- tracks the camera" : "");
        h.travel = travel;
    }

    // ---- time series for the strongest candidates -------------------------
    // A score alone says "close to the camera".  A time series says "moves the
    // same way the camera moves", which is the claim actually being made.
    printf("\n=== time series for the strongest candidates ===\n");
    printf("(one line per ~%d ms; camera first, then each candidate)\n",
           kSamples / 10 * kSleepMs);
    int series = 0;
    for (const auto& h : hits) {
        if (series++ >= 4) break;
        if (h.nearC < kSamples / 3) break;
        printf("\n  %s +0x%04X\n", objs[h.obj].name, h.off);
        printf("      camera");
        for (int i = 0; i < kSamples; i += kSamples / 10)
            printf(" %8.2f", cams[i][0]);
        printf("\n      value ");
        for (int i = 0; i < kSamples; i += kSamples / 10) {
            float p[3];
            memcpy(p, &snap[h.obj][i][h.off], 12);
            printf(" %8.2f", p[0]);
        }
        printf("\n      dy    ");
        for (int i = 0; i < kSamples; i += kSamples / 10) {
            float p[3];
            memcpy(p, &snap[h.obj][i][h.off], 12);
            printf(" %8.2f", p[1]);
        }
        printf("\n      dz    ");
        for (int i = 0; i < kSamples; i += kSamples / 10) {
            float p[3];
            memcpy(p, &snap[h.obj][i][h.off], 12);
            printf(" %8.2f", p[2]);
        }
        printf("\n");
    }

    printf("\n=== what actually changed ===\n");
    //  Ask the memory what is live, instead of asking the documentation what
    //  should be.  For every float3 slot in every sampled object, report the
    //  largest change seen across the capture.  The Camera is included as a
    //  control: its position field is known good, so it calibrates the whole
    //  mechanism.  If Camera shows up and nothing else does, then the other four
    //  objects genuinely hold no live state, and no choice of offset will fix
    //  that -- the chain would be pointing at the wrong objects.
    struct Change { size_t obj; uint32_t off; float maxDelta; float last[3]; };
    std::vector<Change> changes;
    for (size_t o = 0; o < objs.size(); o++)
        for (uint32_t off = 0; off + 12 <= kSpan; off += 4) {
            float p0[3];
            memcpy(p0, &snap[o][0][off], 12);
            if (!std::isfinite(p0[0]) || !std::isfinite(p0[1]) || !std::isfinite(p0[2])) continue;
            float maxd = 0, lastv[3] = { 0,0,0 };
            bool any = false;
            for (int i = 0; i < kSamples; i++) {
                float p[3];
                memcpy(p, &snap[o][i][off], 12);
                if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
                float dx = p[0]-p0[0], dy = p[1]-p0[1], dz = p[2]-p0[2];
                float d = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (d > maxd) maxd = d;
                memcpy(lastv, p, 12);
                any = true;
            }
            if (!any || maxd < 0.5f) continue;
            changes.push_back({ o, off, maxd, { lastv[0], lastv[1], lastv[2] } });
        }
    std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) {
        return a.maxDelta > b.maxDelta; });
    printf("%-16s %-8s %-10s %s\n", "object", "offset", "max change", "last value");
    for (size_t i = 0; i < changes.size() && i < 30; i++) {
        const Change& c = changes[i];
        printf("%-16s +0x%04X  %9.2f  %9.2f %9.2f %9.2f%s\n",
               objs[c.obj].name, c.off, c.maxDelta, c.last[0], c.last[1], c.last[2],
               std::string(objs[c.obj].name) == "Camera" ? "   <- control" : "");
    }
    if (changes.empty())
        printf("    nothing changed anywhere.  Not even the camera -- the capture\n"
               "    mechanism itself failed.\n");

    // Did the control behave?
    bool camLive = false, playerLive = false;
    for (const auto& c : changes) {
        std::string n = objs[c.obj].name;
        if (n == "Camera" && c.maxDelta > 5.f) camLive = true;
        if (n != "Camera" && c.maxDelta > 5.f) playerLive = true;
    }
    printf("\n=== result ===\n");
    printf("  camera changed by more than 5 units: %s   (control)\n", camLive ? "yes" : "NO");
    printf("  any player object changed by more than 5 units: %s\n", playerLive ? "yes" : "NO");
    if (camLive && !playerLive) {
        printf("\n  The capture works -- the camera is demonstrably live -- but none of\n");
        printf("  LocalPlayer, ActorInstance, ActorProps, Component or IEntity holds\n");
        printf("  changing data.  Those objects are not the live actor graph, whatever\n");
        printf("  the chain resolves to.  The next step is not another offset: it is\n");
        printf("  finding which object the game is actually writing, by sweeping for\n");
        printf("  float3 fields that track the camera rather than assuming a path.\n");
        return 4;
    }
    if (!playerLive) {
        printf("\n  not enough movement to identify a field.  Move during the capture.\n");
        return 4;
    }

    for (const auto& h : hits) {
        if (h.travel > 1.f && h.nearC >= kSamples / 3) {
            printf("\n  POSITION FIELD: %s + 0x%X  (%d/%d samples within %.0f units of the "
                   "camera, moved %.1f units)\n",
                   objs[h.obj].name, h.off, h.nearC, kSamples, (double)kNearCamera, h.travel);
            printf("  Read it as three consecutive floats.\n");
            return 0;
        }
    }
    printf("\n  player objects hold live data, but no float3 both moved and stayed\n");
    printf("  near the camera.  See the change table above for where the motion is.\n");
    return 0;
}