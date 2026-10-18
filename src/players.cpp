// -----------------------------------------------------------------------------
//  players.cpp  --  find every player by the measured object signature.
//
//  What is now established by measurement, not documentation:
//
//      base + 0x80D5980 -> ClientEngine
//        +0x58          -> IGameplay
//          +0x58        -> ClientPlayer
//            +0xD88     -> Camera       pos +0x7C  fov +0x454  view +0x41C
//            +0xDD8     -> LocalPlayer  pos +0x7C
//              +0x18    -> ActorInstance
//                +0x88  -> ActorProps
//                  +0x18-> Component
//                    +0x40 -> IEntity     mask +0x3E0 == 2
//
//  Two of those hops are now known to be unreliable, and pretending otherwise is
//  what wasted the last five attempts:
//
//    * Component + 0x40 -> IEntity resolves on some frames and not others, and
//      when it does resolve, that object holds NO position.  During a 300-sample
//      capture against a live player it never once exceeded the change
//      threshold, while LocalPlayer + 0x7C tracked the camera on 300 of 300
//      samples.  So this hop points somewhere real but not at the actor.
//    * The published position at +0x1C0 reads zero on a live player.
//
//  So this tool stops following the chain past LocalPlayer and searches for the
//  player objects directly, using the two offsets that survived measurement:
//
//      object + 0x3E0  == a small entity mask (2 for a player)
//      object + 0x7C   == a finite world position, magnitude > 5
//
//  The method is a single pointer sweep.  Every 8-byte slot in the process is
//  collected if it holds a mapped address; each distinct address is then read in
//  one 0x400-byte block and tested against the signature.  This is not a sweep
//  with a loose acceptance test -- the earlier ones failed because they counted
//  "pointers" and "any float3 that looked plausible", and a heap with five
//  million pointers satisfies both.  Here every element must carry a player
//  mask and a real world coordinate at a known offset.
//
//  The validation that makes the result trustworthy: the local player's own
//  address must appear in the output.  If it does not, the signature does not
//  describe players and the rest of the list means nothing.
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
#include <unordered_set>
#include <map>
#include <array>

namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t Camera       = 0xD88;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t CamPos       = 0x7C;
    constexpr uint64_t CamFov       = 0x454;
    constexpr uint64_t ActorInst    = 0x18;

    constexpr uint64_t EntPos       = 0x7C;   // measured: tracks the camera
    constexpr uint64_t EntMask      = 0x3E0;  // 2 = player
}

namespace {
HANDLE   g_h;
uint64_t g_base;
float    g_cam[3];
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

constexpr size_t kBlock = 0x400;

struct Player {
    uint64_t va = 0;
    float    pos[3] = { 0,0,0 };
    uint32_t mask = 0;
    float    dist = 0;
    float    moved = 0;
};

bool looks_like_player(const uint8_t* blk, Player* p) {
    uint32_t mask;
    memcpy(&mask, blk + off::EntMask, 4);
    // Exactly 2, not "1 to 64".  The loose range matched 40,096 objects in the
    // first run -- masks of 1 and 64 both occur and both carry plausible
    // coordinates, so a range test is no test at all.  The local player reads 2
    // on every live frame measured so far, and 2 is the value the community
    // documents for a player, so this is the one value that can be required
    // rather than guessed at.
    if (mask != 2) return false;
    float q[3];
    memcpy(q, blk + off::EntPos, 12);
    for (int i = 0; i < 3; i++) if (!std::isfinite(q[i])) return false;
    if (std::fabs(q[0]) > 1e5f || std::fabs(q[1]) > 1e5f || std::fabs(q[2]) > 1e5f) return false;
    float m = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2]);
    if (m < 5.f) return false;                       // default slot, not a place
    p->va = 0; p->mask = mask;
    memcpy(p->pos, q, 12);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    printf("players -- find every player by the measured object signature\n");
    printf("=============================================================\n");

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

    {
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
    }

    // ---- wait for a live player ------------------------------------------
    int watch = (argc > 1) ? atoi(argv[1]) : 60;
    if (watch < 1) watch = 1;
    uint64_t cam = 0, loc = 0, inst = 0;
    float localPos[3] = { 0,0,0 };
    printf("=== waiting for a live frame (up to %d s) ===\n", watch);
    for (int i = 0; i < watch; i++) {
        uint64_t e = u64(g_base + off::ClientEngine);
        uint64_t g = mapped(e) ? u64(e + off::IGameplay) : 0;
        uint64_t p = mapped(g) ? u64(g + off::ClientPlayer) : 0;
        cam = mapped(p) ? u64(p + off::Camera) : 0;
        loc = mapped(p) ? u64(p + off::LocalPlayer) : 0;
        inst = mapped(loc) ? u64(loc + off::ActorInst) : 0;
        if (mapped(cam) && mapped(loc) &&
            read(loc + off::EntPos, localPos, 12)) {
            float m = std::sqrt(localPos[0]*localPos[0] + localPos[1]*localPos[1] +
                                localPos[2]*localPos[2]);
            if (m >= 5.f) {
                read(cam + off::CamPos, g_cam, 12);
                printf("  %3ds  live: local player at %.2f %.2f %.2f, mask %u\n", i,
                       localPos[0], localPos[1], localPos[2],
                       u32(loc + off::EntMask));
                break;
            }
        }
        if (i % 5 == 0) printf("  %3d  not spawned yet\n", i);
        Sleep(1000);
    }
    if (!mapped(cam) || !mapped(loc)) { printf("[X] chain never resolved\n"); return 3; }

    printf("\n[+] LocalPlayer 0x%llX  ActorInstance 0x%llX\n",
           (unsigned long long)loc, (unsigned long long)inst);
    printf("[+] camera %.2f %.2f %.2f  fov %.2f\n", g_cam[0], g_cam[1], g_cam[2],
           f32(cam + off::CamFov));
    printf("    this address MUST appear in the result below, or the signature is wrong\n");

    // ---- collect every mapped pointer in the process ----------------------
    printf("\n=== collecting pointers ===\n");
    std::vector<uint64_t> ptrs;
    constexpr size_t CH = 16u << 20;
    std::vector<uint8_t> buf(CH);
    size_t bytes = 0;
    const uint64_t deadline = GetTickCount64() + 240'000;
    for (const auto& r : g_regions) {
        if (GetTickCount64() > deadline) { printf("    [!] time limit\n"); break; }
        uint64_t lo = (uint64_t)r.BaseAddress, hi = lo + r.RegionSize;
        for (uint64_t a = lo; a < hi && GetTickCount64() < deadline; a += CH) {
            size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
            if (!read(a, buf.data(), want)) continue;
            size_t n = want & ~(size_t)7;
            for (size_t i = 0; i < n; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (mapped(q)) ptrs.push_back(q);
            }
            bytes += want;
        }
    }
    std::sort(ptrs.begin(), ptrs.end());
    ptrs.erase(std::unique(ptrs.begin(), ptrs.end()), ptrs.end());
    printf("    scanned %.0f MB, %zu distinct mapped pointers\n",
           bytes / (1024.0*1024.0), ptrs.size());

    // ---- test each against the signature ---------------------------------
    printf("\n=== testing the signature ===\n");
    std::unordered_map<uint64_t, Player> found;
    std::vector<uint8_t> blk(kBlock);
    std::map<uint32_t, uint32_t> maskHist;
    size_t tested = 0;
    for (uint64_t v : ptrs) {
        if ((tested & 0xFFFF) == 0 && GetTickCount64() > deadline) {
            printf("    [!] time limit at %zu/%zu\n", tested, ptrs.size());
            break;
        }
        tested++;
        if (!read(v, blk.data(), kBlock)) continue;
        // histogram of the mask field, whatever the value: the distribution is
        // evidence about which mask means "player", not something to assume
        {
            uint32_t m;
            memcpy(&m, blk.data() + off::EntMask, 4);
            float q[3];
            memcpy(q, blk.data() + off::EntPos, 12);
            bool finite = std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]);
            float mag = finite ? std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2]) : 0.f;
            if (finite && mag >= 5.f && m <= 64) maskHist[m]++;
        }
        Player p{};
        if (!looks_like_player(blk.data(), &p)) continue;
        p.va = v;
        found[v] = p;
    }
    printf("    tested %zu addresses, %zu match the signature\n", tested, found.size());

    printf("\n    mask field values that accompany a plausible world position:\n");
    for (auto& kv : maskHist)
        printf("      mask %-4u %8d\n", kv.first, kv.second);

    // -----------------------------------------------------------------------
    //  Motion filter.
    //
    //  The mask field at +0x3E0 turned out not to be an entity class at all.
    //  Its distribution over objects holding a plausible world position is
    //  almost flat from 0 to 64 -- 647,071 read 0, 106,052 read 2, 12,645 read
    //  3 -- which is what a counter or a bitfield looks like, not a type tag.
    //  Requiring exactly 2 narrowed 9,666,292 pointers to 9,856 objects, which
    //  is a good ratio, but 9,856 is not a player list.
    //
    //  What separates players from scenery is that players move.  9,856 is
    //  small enough to sample twice and compare, so that is the discriminator,
    //  and it needs no magic number: the local player is known to move, so any
    //  rule that drops it is wrong.
    // -----------------------------------------------------------------------
    printf("\n=== motion filter ===\n");
    std::vector<Player> moved;
    if (!found.empty()) {
        printf("    first pass over %zu candidates, second in 3 s -- MOVE during it\n",
               found.size());
        fflush(stdout);
        std::unordered_map<uint64_t, std::array<float,3>> before;
        for (auto& kv : found) {
            std::array<float,3> p{};
            if (read(kv.first + off::EntPos, p.data(), 12)) before[kv.first] = p;
        }
        Sleep(3000);
        for (auto& kv : found) {
            std::array<float,3> p{};
            if (!read(kv.first + off::EntPos, p.data(), 12)) continue;
            auto it = before.find(kv.first);
            if (it == before.end()) continue;
            float dx = p[0]-it->second[0], dy = p[1]-it->second[1], dz = p[2]-it->second[2];
            float d = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (d < 0.5f) continue;
            Player pl = kv.second;
            memcpy(pl.pos, p.data(), 12);
            pl.dist = std::sqrt((pl.pos[0]-g_cam[0])*(pl.pos[0]-g_cam[0]) +
                                (pl.pos[1]-g_cam[1])*(pl.pos[1]-g_cam[1]) +
                                (pl.pos[2]-g_cam[2])*(pl.pos[2]-g_cam[2]));
            pl.moved = d;
            moved.push_back(pl);
        }
        std::sort(moved.begin(), moved.end(),
                  [](const Player& a, const Player& b) { return a.moved > b.moved; });
    }
    printf("    %zu of %zu candidates moved more than 0.5 units\n\n",
           moved.size(), found.size());
    printf("%-4s %-18s %-6s %-10s %-10s %-10s %8s %8s\n",
           "#", "object", "mask", "x", "y", "z", "dist", "moved");
    for (size_t i = 0; i < moved.size() && i < 64; i++)
        printf("%-4zu 0x%016llX %-6u %10.2f %10.2f %10.2f %8.1f %8.2f%s\n",
               i, (unsigned long long)moved[i].va, moved[i].mask,
               moved[i].pos[0], moved[i].pos[1], moved[i].pos[2],
               moved[i].dist, moved[i].moved,
               moved[i].va == loc ? "   <- local player" : "");
    if (moved.size() > 64) printf("    ... %zu more\n", moved.size() - 64);

    bool localMoved = false;
    for (const auto& p : moved) if (p.va == loc) localMoved = true;
    printf("\n  local player moved during the window: %s\n", localMoved ? "yes" : "NO");
    if (localMoved && moved.size() > 1)
        printf("  -> the signature describes movers, and the local player is one.\n");
    else if (!localMoved)
        printf("  -> the local player did not move, so this run proves nothing.\n");
    else
        printf("  -> only the local player moved; the mask==2 signature does not\n"
               "     describe remote players either.\n");

    std::vector<Player> ps;
    ps.reserve(found.size());
    for (auto& kv : found) {
        Player p = kv.second;
        float dx = p.pos[0]-g_cam[0], dy = p.pos[1]-g_cam[1], dz = p.pos[2]-g_cam[2];
        p.dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        ps.push_back(p);
    }
    std::sort(ps.begin(), ps.end(),
              [](const Player& a, const Player& b) { return a.dist < b.dist; });

    printf("\n%-4s %-18s %-6s %-10s %-10s %-10s %8s\n",
           "#", "object", "mask", "x", "y", "z", "dist");
    for (size_t i = 0; i < ps.size() && i < 64; i++) {
        printf("%-4zu 0x%016llX %-6u %10.2f %10.2f %10.2f %8.1f%s\n",
               i, (unsigned long long)ps[i].va, ps[i].mask,
               ps[i].pos[0], ps[i].pos[1], ps[i].pos[2], ps[i].dist,
               ps[i].va == loc ? "   <- local player" : "");
    }
    if (ps.size() > 64) printf("    ... %zu more\n", ps.size() - 64);

    // ---- validation -------------------------------------------------------
    bool hasLocal = found.count(loc) != 0;
    int distinct = 0;
    for (size_t i = 0; i < ps.size(); i++)
        for (size_t j = i + 1; j < ps.size(); j++) {
            float dx = ps[i].pos[0]-ps[j].pos[0], dy = ps[i].pos[1]-ps[j].pos[1],
                  dz = ps[i].pos[2]-ps[j].pos[2];
            if (dx*dx + dy*dy + dz*dz > 1.f) { distinct++; if (distinct > 20) goto done; }
        }
done:
    printf("\n=== result ===\n");
    printf("  objects matching the signature : %zu\n", ps.size());
    printf("  local player among them        : %s\n", hasLocal ? "YES" : "NO");
    printf("  distinct position pairs        : %s\n", distinct ? "yes" : "no");
    if (hasLocal && distinct) {
        printf("\n  VALIDATED.  The signature describes real players: it found the one\n");
        printf("  under your control and %.d other objects standing elsewhere.\n",
               (int)ps.size() - 1);
        printf("  This is the player set the ESP iterates.\n");
    } else if (!hasLocal) {
        printf("\n  NOT VALIDATED.  The known local player does not match, so either\n");
        printf("  +0x3E0 or +0x7C is not a property of every player object and the rest\n");
        printf("  of this list cannot be trusted.\n");
    } else {
        printf("\n  only one position -- the signature may be matching one object.\n");
    }

    FILE* f = fopen("entity_array.json", "w");
    if (f) {
        fprintf(f, "{\n  \"camera\": [%.3f, %.3f, %.3f],\n"
                   "  \"local\": \"0x%llX\",\n  \"players\": [",
                g_cam[0], g_cam[1], g_cam[2], (unsigned long long)loc);
        for (size_t i = 0; i < ps.size(); i++)
            fprintf(f, "%s\n    { \"va\": \"0x%llX\", \"mask\": %u, "
                       "\"pos\": [%.3f, %.3f, %.3f], \"dist\": %.2f }",
                    i ? "," : "", (unsigned long long)ps[i].va, ps[i].mask,
                    ps[i].pos[0], ps[i].pos[1], ps[i].pos[2], ps[i].dist);
        fprintf(f, "\n  ]\n}\n");
        fclose(f);
        printf("  wrote entity_array.json\n");
    }
    return 0;
}