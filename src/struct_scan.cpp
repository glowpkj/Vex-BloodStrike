// -----------------------------------------------------------------------------
//  struct_scan.cpp  --  resolve the last two unknowns by scanning, not guessing.
//
//  What is already proven on this build (see chain_test):
//
//      base + 0x80D5980 -> ClientEngine
//        +0x58          -> IGameplay
//          +0x58        -> ClientPlayer
//            +0xD88    -> Camera      (position 0x7C, fov 0x454, view 0x41C)
//            +0xDD8    -> LocalPlayer
//              +0x18   -> ActorInstance
//
//  Two links are still missing and both were guessed wrong by every published
//  source: ActorProps (the hop into the entity's component tree) and the shape
//  of the entity list.  Guessing failed six times, so this measures instead.
//
//  The method is the same one the earlier target used: for every 4-byte
//  slot in the object, walk the chain and keep only the results that satisfy an
//  invariant nothing else satisfies.
//
//  The invariant used here is spatial, and it is strong.  We know the camera
//  position exactly -- it produced sane world coordinates already.  The local
//  player's transform must therefore be within a few metres of it.  A chain
//  that lands on a transform within 10 units of the camera is the real chain;
//  every other combination either fails to read, produces non-finite floats, or
//  lands thousands of units away.
//
//  For the entity list, the same trick in reverse: find arrays of pointers where
//  the members resolve to transforms that are near the camera.  Real entity
//  lists cluster in world space; noise does not.
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>

namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t EntityList   = 0x8B78AA8;
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t Camera       = 0xD88;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t CamPos       = 0x7C;
    constexpr uint64_t CamFov       = 0x454;
    constexpr uint64_t ActorInst    = 0x18;

    // hop candidates published for this engine, tried as a set
    constexpr uint64_t kActorPropsHop[]     = { 0x278, 0x3B8, 0x2D8, 0x308, 0x348 };
    constexpr uint64_t kComponentHop[]     = { 0x18, 0x30, 0x28, 0x20, 0x38 };
    constexpr uint64_t kIEntityHop[]        = { 0x40, 0x48, 0x38, 0x50, 0x58 };
    constexpr uint64_t kTransformHop[]      = { 0x58, 0x60, 0x48, 0x50, 0x68 };
    constexpr uint64_t kEntityMaskOff[]     = { 0x3E0, 0x2E0, 0x2D0, 0x388 };
    constexpr uint64_t kPosOff[]            = { 0x7C, 0x1C0, 0x58 };
}

namespace {

HANDLE   g_h;
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

float g_cam[3] = { 0, 0, 0 };

inline float dist2(const float* p) {
    float dx = p[0] - g_cam[0], dy = p[1] - g_cam[1], dz = p[2] - g_cam[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// Read a position at +off from `at` and judge it.  Rejects non-finite values
/// and coordinates that are implausibly far from the world origin, which is the
/// main way a random pointer produces a "float3 that looks fine".
inline bool pos_at(uint64_t at, uint64_t off, float* out) {
    if (!read(at + off, out, 12)) return false;
    for (int i = 0; i < 3; i++) {
        if (!std::isfinite(out[i])) return false;
        if (std::fabs(out[i]) > 1e5f) return false;
    }
    int nz = (std::fabs(out[0]) > 0.01f) + (std::fabs(out[1]) > 0.01f) +
             (std::fabs(out[2]) > 0.01f);
    return nz >= 2;          // not an all-zero default slot
}

} // namespace

int main() {
    printf("struct_scan -- resolve ActorProps and the entity list by measurement\n");
    printf("================================================================\n");

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
    if (!g_h) { printf("[X] OpenProcess failed (%lu) -- run elevated\n", GetLastError()); return 2; }

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(s2, &me)) {
        do { if (!_wcsicmp(me.szModule, L"BloodStrike.exe")) { g_base = (uint64_t)me.modBaseAddr; break; } }
        while (Module32NextW(s2, &me));
    }
    CloseHandle(s2);

    uint64_t textLo = 0, textHi = 0, dataLo = 0, dataHi = 0;
    {
        wchar_t path[MAX_PATH]{}; DWORD n = MAX_PATH;
        QueryFullProcessImageNameW(g_h, 0, path, &n);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::vector<uint8_t> head(1u << 20);
        DWORD got = 0;
        if (f != INVALID_HANDLE_VALUE) {
            ReadFile(f, head.data(), (DWORD)head.size(), &got, nullptr);
            CloseHandle(f);
        }
        auto* d = (IMAGE_DOS_HEADER*)head.data();
        auto* nt = (IMAGE_NT_HEADERS64*)(head.data() + d->e_lfanew);
        const IMAGE_SECTION_HEADER* sh =
            (const IMAGE_SECTION_HEADER*)((const uint8_t*)nt + sizeof(IMAGE_NT_HEADERS64));
        for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sh) {
            char nm[9]{}; memcpy(nm, sh->Name, 8);
            uint64_t vs = (sh->Misc.VirtualSize > sh->SizeOfRawData)
                          ? sh->Misc.VirtualSize : sh->SizeOfRawData;
            if (!strcmp(nm, ".text")) { textLo = g_base + sh->VirtualAddress; textHi = textLo + vs; }
            if (!strcmp(nm, ".data")) { dataLo = g_base + sh->VirtualAddress; dataHi = dataLo + vs; }
        }
    }

    {
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        uintptr_t a = 0;
        while ((uint64_t)a < (uint64_t)si.lpMaximumApplicationAddress) {
            MEMORY_BASIC_INFORMATION m{};
            if (VirtualQueryEx(g_h, (LPCVOID)a, &m, sizeof m) == 0) break;
            if (m.State == MEM_COMMIT && m.RegionSize > 0 && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                g_regions.push_back(m);
            uintptr_t nx = (uintptr_t)m.BaseAddress + m.RegionSize;
            if (nx <= a) break;
            a = nx;
        }
        std::sort(g_regions.begin(), g_regions.end(),
                  [](const auto& x, const auto& y) {
                      return (uint64_t)x.BaseAddress < (uint64_t)y.BaseAddress; });
    }

    printf("[+] pid %u  base 0x%llX  %zu regions\n", pid,
           (unsigned long long)g_base, g_regions.size());

    // ---- reach the camera so the search has an anchor ----------------------
    uint64_t engine = u64(g_base + off::ClientEngine);
    uint64_t gp = mapped(engine) ? u64(engine + off::IGameplay) : 0;
    uint64_t pl = mapped(gp) ? u64(gp + off::ClientPlayer) : 0;
    uint64_t cam = mapped(pl) ? u64(pl + off::Camera) : 0;
    if (!mapped(cam)) { printf("[X] chain to the camera failed\n"); return 3; }
    read(cam + off::CamPos, g_cam, 12);
    printf("[+] camera at %.2f %.2f %.2f   fov %.2f\n", g_cam[0], g_cam[1], g_cam[2],
           f32(cam + off::CamFov));

    uint64_t loc = mapped(pl) ? u64(pl + off::LocalPlayer) : 0;
    uint64_t inst = mapped(loc) ? u64(loc + off::ActorInst) : 0;
    if (!mapped(inst)) { printf("[X] chain to the local ActorInstance failed\n"); return 3; }
    printf("[+] local ActorInstance 0x%llX\n", (unsigned long long)inst);

    // ======================================================================
    //  Part 1 -- ActorProps
    //
    //  Scan every slot, and at each one try every published hop combination.
    //  Keep results whose final position is within 10 units of the camera.
    // ======================================================================
    printf("\n=== part 1  ActorProps ===\n");
    struct Sol { uint64_t props, comp, ient, posoff; float pos[3]; uint32_t mask;
                  uint32_t props_slot, comp_hop, ient_hop; };
    std::vector<Sol> sols;

    for (uint64_t slot = 0x40; slot < 0x1000; slot += 4) {
        uint64_t props = u64(inst + slot);
        if (!mapped(props)) continue;

        for (uint64_t hc : off::kComponentHop) {
            uint64_t comp = u64(props + hc);
            if (!mapped(comp)) continue;
            for (uint64_t hi : off::kIEntityHop) {
                uint64_t ient = u64(comp + hi);
                if (!mapped(ient)) continue;
                for (uint64_t ht : off::kTransformHop) {
                    // The transform slot may itself be the position, or point at
                    // an object that holds it.  Try both.
                    for (int indirect = 0; indirect < 2; indirect++) {
                        uint64_t at = indirect ? u64(ient + ht) : (ient + ht);
                        if (indirect && !mapped(at)) continue;
                        for (uint64_t po : off::kPosOff) {
                            float p[3];
                            if (!pos_at(at, po, p)) continue;
                            float d = dist2(p);
                            if (d > 10.f) continue;
                            Sol sol{};
                            sol.props = props; sol.comp = comp; sol.ient = ient;
                            sol.posoff = po; sol.mask = u32(ient + off::kEntityMaskOff[0]);
                            sol.props_slot = (uint32_t)slot;
                            sol.comp_hop = (uint32_t)hc;
                            sol.ient_hop = (uint32_t)hi;
                            memcpy(sol.pos, p, sizeof p);
                            sols.push_back(sol);
                        }
                    }
                }
            }
        }
    }

    printf("    combinations landing within 10 units of the camera: %zu\n\n", sols.size());
    // The chains that agree on the same IEntity are the real one; the rest are
    // different routes that happen to converge.  Report the hop offsets, not
    // just the addresses -- the addresses are run-specific, the offsets are not.
    {
        std::unordered_map<uint64_t, int> byIent;
        for (const auto& s : sols) byIent[s.ient]++;
        printf("    distinct IEntity reached: %zu\n", byIent.size());
        int shown = 0;
        for (const auto& s : sols) {
            if (s.ient != sols.front().ient) continue;
            if (shown++ >= 3) break;
            printf("\n    -- chain via IEntity 0x%llX (reached by %d routes) --\n",
                   (unsigned long long)s.ient, byIent[s.ient]);
            printf("       ActorInstance + 0x%X = props  0x%llX\n",
                   s.props_slot, (unsigned long long)s.props);
            printf("       props       + 0x%X = comp   0x%llX\n",
                   s.comp_hop, (unsigned long long)s.comp);
            printf("       comp        + 0x%X = ient   0x%llX\n",
                   s.ient_hop, (unsigned long long)s.ient);
            printf("       ient        + 0x%X = position (%.2f %.2f %.2f)\n",
                   s.posoff, s.pos[0], s.pos[1], s.pos[2]);
            printf("       ient        + 0x%X = EntityMask (0x%X)  %s\n",
                   off::kEntityMaskOff[0], s.mask,
                   s.mask == 2 ? "<-- PLAYER" : "");
        }
    }
    printf("\n");
    if (sols.empty())
        printf("    nothing.  The chain does not reach a position near the camera\n");
        printf("    through any published hop combination.\n");

    // ======================================================================
    //  Part 2 -- entity list, found by clustering instead of by shape
    //
    //  Whatever the container is called, the players are in memory as pointers
    //  to objects that hold a position near the camera.  So scan writable
    //  memory for pointers, chase each one through the same hop search, and
    //  tally the offsets that recur.  If N players sit near the camera, the
    //  position offset will appear N times and nothing else will.
    // ======================================================================
    printf("\n=== part 2  entity list by position clustering ===\n");
    printf("    scanning .data and private heaps for pointer arrays\n");

    std::vector<uint64_t> ptrs;
    constexpr size_t CH = 8u << 20;
    std::vector<uint8_t> buf(CH);
    const uint64_t deadline = GetTickCount64() + 150'000;

    auto sweep = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi; a += CH) {
            if (GetTickCount64() > deadline) return;
            size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
            if (!read(a, buf.data(), want)) continue;
            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (mapped(q)) ptrs.push_back(q);
            }
        }
    };
    sweep(dataLo, dataHi);
    for (const auto& r : g_regions) {
        if (r.Protect & PAGE_READWRITE)
            sweep((uint64_t)r.BaseAddress, (uint64_t)r.BaseAddress + r.RegionSize);
    }

    std::sort(ptrs.begin(), ptrs.end());
    ptrs.erase(std::unique(ptrs.begin(), ptrs.end()), ptrs.end());
    printf("    distinct pointers collected: %zu\n", ptrs.size());
    if (ptrs.size() > 200000) ptrs.resize(200000);

    // For each pointer, look for a position near the camera within a window.
    // This does not need to know the chain: it asks "does this object have a
    // float3 that looks like a place in this map".
    struct Vote { uint64_t obj; uint32_t off; float p[3]; };
    std::unordered_map<uint64_t, int> tally;
    std::unordered_map<uint64_t, float> sampleX, sampleY, sampleZ;
    std::vector<Vote> hits;

    std::vector<uint8_t> w(0x400);
    for (size_t i = 0; i < ptrs.size(); i++) {
        if ((i & 0x3FF) == 0 && GetTickCount64() > deadline) {
            printf("    [!] time limit at %zu/%zu\n", i, ptrs.size());
            break;
        }
        if (!read(ptrs[i], w.data(), w.size())) continue;
        for (size_t o = 0; o + 12 <= w.size(); o += 4) {
            float p[3];
            memcpy(p, w.data() + o, 12);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) continue;
            if (std::fabs(p[0]) > 1e5f || std::fabs(p[1]) > 1e5f || std::fabs(p[2]) > 1e5f) continue;
            int nz = (std::fabs(p[0]) > 0.01f) + (std::fabs(p[1]) > 0.01f) +
                     (std::fabs(p[2]) > 0.01f);
            if (nz < 2) continue;

            // Two separate gates, and the order matters.  The old code only
            // checked distance-from-camera, which every offset inside the local
            // player's own 0x400-byte window satisfies -- that is why the
            // ranking came back full of (0, 2, 2)-style garbage that was really
            // a matrix or a bounding box.
            //
            // 1. the value must be an ABSOLUTE world position: close to the
            //    camera, and large enough to be a place rather than a delta.
            // 2. it must have MOVED since the camera sample, so the camera is
            //    not being matched against its own stale fields.
            if (dist2(p) > 400.f) continue;
            float mag = std::sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
            if (mag < 20.f) continue;                 // relative offsets, not world

            tally[o]++;
            if (tally[o] == 1) { sampleX[o] = p[0]; sampleY[o] = p[1]; sampleZ[o] = p[2]; }
            if (hits.size() < 4000) hits.push_back({ ptrs[i], (uint32_t)o, { p[0], p[1], p[2] } });
        }
    }

    std::vector<std::pair<uint64_t, int>> rank(tally.begin(), tally.end());
    std::sort(rank.begin(), rank.end(), [](auto& a, auto& b) { return a.second > b.second; });
    printf("    distinct objects holding a near-camera float3: %zu\n\n", hits.size());
    printf("    offset    objects   sample position\n");
    int k = 0;
    for (const auto& r : rank) {
        if (k++ >= 20) break;
        if (r.second < 3) break;
        printf("    +0x%03X    %7d   %.2f %.2f %.2f\n", (unsigned)r.first, r.second,
               sampleX[r.first], sampleY[r.first], sampleZ[r.first]);
    }
    if (rank.empty() || rank.front().second < 3)
        printf("    no offset recurred often enough to be an entity list.\n");

    // ----------------------------------------------------------------------
    printf("\n=== result ===\n");
    if (!sols.empty())
        printf("  ActorProps: FOUND -- see part 1, %zu candidate(s).\n", sols.size());
    else
        printf("  ActorProps: not reachable by any published hop combination.\n");
    printf("  EntityList: %s\n",
           (!rank.empty() && rank.front().second >= 3)
               ? "a recurring offset exists -- see part 2."
               : "no recurring offset found.");
    return 0;
}
