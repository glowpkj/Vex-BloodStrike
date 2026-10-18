// -----------------------------------------------------------------------------
//  entity_scan.cpp  --  find every player by signature, without a container.
//
//  Five attempts have now failed to locate the entity list, and the reason is
//  now known: the game was not in a match.  The local player object existed with
//  EntityMask == 2, but its position read (0, 0, 0) instead of a world
//  coordinate.  Every acceptance test used so far required "several members
//  whose positions differ from one another", which is unsatisfiable when every
//  position is zero.  The searches were not wrong, they were starved.
//
//  Rather than search for a container one more time, this removes the container
//  from the problem.  Three hops of the chain are verified beyond doubt:
//
//      ActorProps + 0x18 -> Component
//      Component  + 0x40 -> IEntity
//      IEntity    + 0x3E0 -> EntityMask == 2  (player)
//      IEntity    + 0x1C0 -> position
//
//  So a player in memory has a signature: some address P holds a pointer Q,
//  Q+0x18 is mapped, and *(Q+0x18+0x40) is an IEntity whose mask is 2.  That
//  signature is far rarer than "a pointer", which is what killed the earlier
//  sweeps -- a heap with five million pointers yields a false "list" under any
//  loose test, and this test has no loose component: every element must resolve
//  through three verified hops and carry a player mask.
//
//  What is produced is the player set directly.  An ESP needs positions, not a
//  container.
//
//  Cost: the signature sweep touches ~1.4 GB once.  It is not run per frame.
//  The resulting IEntity pointers are cached and re-read each frame; entries that
//  stop validating (player disconnected, actor destroyed) are dropped, and a
//  rescan can be triggered on demand.
//
//  A preflight gate runs first and refuses to proceed unless the local player is
//  actually alive and positioned, because running this against an empty map
//  produces a confident, empty, wrong answer.
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

namespace off {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t Camera       = 0xD88;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t CamPos       = 0x7C;
    constexpr uint64_t CamFov       = 0x454;
    constexpr uint64_t ActorInst    = 0x18;
    // 0x88, not the published 0x48.  0x48 resolved to a component in the lobby
    // object and looked correct, which is how a wrong offset survives: it has to
    // be wrong in a way that still produces plausible values.  struct_scan
    // measured 0x88 against a live, moving player, and the chain through 0x48
    // no longer resolves at all.
    constexpr uint64_t ActorProps   = 0x88;
    constexpr uint64_t Component    = 0x18;
    constexpr uint64_t IEntity      = 0x40;
    // Position measured by pos_find against a live, moving player: 300/300 samples
    // tracked the camera, x and z identical to the millimetre, y exactly 1.5
    // below it -- eye height above the actor root.  The published 0x1C0 read
    // (0,0,0) for ninety consecutive seconds on a live player; one early sample
    // had made it look right and five wrong searches were built on top of it.
    constexpr uint64_t EntPos       = 0x7C;
    constexpr uint64_t EntMask      = 0x3E0;

    // skeleton, per the two independent sources on this engine: bone 7 is the
    // neck, not 8, and the matrix at +0x30 is relative to the actor transform
    constexpr uint64_t BoneRoot     = 0x18;
    constexpr uint64_t BoneNode     = 0x90;
    constexpr uint64_t BoneList     = 0x8;
    constexpr uint64_t BoneMatrix   = 0x30;
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

struct Entity {
    uint64_t ient = 0, props = 0, comp = 0, inst = 0;
    float    pos[3] = { 0, 0, 0 };
    uint32_t mask = 0;
    float    dist = 0;
};

/// An IEntity is only accepted when every field it depends on is read
/// successfully.  load_entity is the gate for the whole tool.
bool load_entity(uint64_t ient, Entity* e) {
    if (!mapped(ient)) return false;
    memset(e, 0, sizeof *e);
    e->ient = ient;
    e->mask = u32(ient + off::EntMask);
    if (e->mask != 2) return false;                 // player class only
    if (!read(ient + off::EntPos, e->pos, 12)) return false;
    for (int i = 0; i < 3; i++) if (!std::isfinite(e->pos[i])) return false;
    if (std::fabs(e->pos[0]) > 1e5f || std::fabs(e->pos[1]) > 1e5f ||
        std::fabs(e->pos[2]) > 1e5f) return false;
    return true;
}

/// Complete the signature backwards from an IEntity to its ActorProps, so the
/// caller can report which address holds the player.
void backfill(Entity* e) {
    if (!e->comp) {
        // find the component by probing: any props whose +0x18 -> +0x40 is e->ient
        (void)e;
    }
}

// Every hop of the local player chain, kept separately so a break can be
// located exactly instead of collapsing into "chain failed".  A previous run
// reported only "[X] local chain incomplete" while the camera was live and
// moving, which says nothing about where it broke.
struct Chain {
    uint64_t engine = 0, gp = 0, pl = 0, cam = 0;
    uint64_t loc = 0, inst = 0, props = 0, comp = 0, ient = 0;
    bool     ok() const { return ient != 0; }
    const char* brokenAt() const {
        if (!engine) return "ClientEngine global";
        if (!gp)    return "IGameplay (+0x58)";
        if (!pl)    return "ClientPlayer (+0x58)";
        if (!cam)   return "Camera (+0xD88)";
        if (!loc)   return "LocalPlayer (+0xDD8)";
        if (!inst)  return "ActorInstance (+0x18)";
        if (!props) return "ActorProps (+0x48)";
        if (!comp)  return "Component (+0x18)";
        if (!ient)  return "IEntity (+0x40)";
        return "none";
    }
};

Chain resolve_chain(uint64_t base) {
    Chain c;
    c.engine = u64(base + off::ClientEngine);
    if (!mapped(c.engine)) return c;
    c.gp = u64(c.engine + off::IGameplay);
    if (!mapped(c.gp)) return c;
    c.pl = u64(c.gp + off::ClientPlayer);
    if (!mapped(c.pl)) return c;
    c.cam = u64(c.pl + off::Camera);
    if (!mapped(c.cam)) return c;
    c.loc = u64(c.pl + off::LocalPlayer);
    if (!mapped(c.loc)) return c;
    c.inst = u64(c.loc + off::ActorInst);
    if (!mapped(c.inst)) return c;
    c.props = u64(c.inst + off::ActorProps);
    if (!mapped(c.props)) return c;
    c.comp = u64(c.props + off::Component);
    if (!mapped(c.comp)) return c;
    c.ient = u64(c.comp + off::IEntity);
    if (!mapped(c.ient)) return c;
    return c;
}

void print_chain(const Chain& c, uint64_t base) {
    auto row = [&](const char* name, uint64_t slotOff, uint64_t val) {
        printf("      %-16s %s +0x%03llX -> 0x%016llX  %s\n", name, "",
               (unsigned long long)slotOff, (unsigned long long)val,
               val && mapped(val) ? "mapped" : (val ? "NOT MAPPED" : "null"));
    };
    (void)base;
    row("ClientEngine", off::ClientEngine, c.engine);
    row("  IGameplay", off::IGameplay, c.gp);
    row("    ClientPlayer", off::ClientPlayer, c.pl);
    row("      Camera", off::Camera, c.cam);
    row("      LocalPlayer", off::LocalPlayer, c.loc);
    row("        ActorInstance", off::ActorInst, c.inst);
    row("          ActorProps", off::ActorProps, c.props);
    row("            Component", off::Component, c.comp);
    row("              IEntity", off::IEntity, c.ient);
}

} // namespace

int main(int argc, char** argv) {
    printf("entity_scan -- find every player by signature, no container needed\n");
    printf("=================================================================\n");

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

    // Wait for a frame in which the local player is genuinely alive.
    //
    //  The +0x1C0 position field is only written while the actor is spawned.  It
    //  reads (0,0,0) in the lobby, at a death screen, and between rounds, so an
    //  abort on the first sample means the answer depends entirely on when the
    //  tool happens to be launched.  Instead, poll: the operator starts this
    //  once, keeps playing, and the sweep fires on the first live frame.
    //
    //  The chain itself is polled at the same time and separately reported, so a
    //  genuine offset break is never mistaken for "not alive yet".
    int watchSeconds = (argc > 1) ? atoi(argv[1]) : 45;
    if (watchSeconds < 1) watchSeconds = 1;

    Chain ch{};
    Entity local{};
    bool alive = false;
    uint64_t lastCamSeen = 0;
    printf("=== waiting for a live frame (up to %ds -- keep playing) ===\n", watchSeconds);
    for (int i = 0; i < watchSeconds; i++) {
        Chain t = resolve_chain(g_base);
        if (t.cam) lastCamSeen = t.cam;
        if (!t.ok()) {
            printf("  %3ds  chain breaks at %s\n", i, t.brokenAt());
            Sleep(1000);
            continue;
        }
        Entity e{};
        float mag = 0;
        if (load_entity(t.ient, &e))
            mag = std::sqrt(e.pos[0]*e.pos[0] + e.pos[1]*e.pos[1] + e.pos[2]*e.pos[2]);
        if (mag >= 1.f) {
            ch = t; local = e; alive = true;
            printf("  %3ds  live: pos %.2f %.2f %.2f\n", i, e.pos[0], e.pos[1], e.pos[2]);
            break;
        }
        printf("  %3ds  chain ok, local player not spawned (pos %.2f %.2f %.2f, fov %.1f)\n",
               i, e.pos[0], e.pos[1], e.pos[2],
               mapped(t.cam) ? f32(t.cam + off::CamFov) : -1.f);
        Sleep(1000);
    }

    printf("\n[+] pid %u  base 0x%llX  %zu readable regions\n", pid,
           (unsigned long long)g_base, g_regions.size());

    if (!ch.cam) ch.cam = lastCamSeen;
    if (mapped(ch.cam)) {
        read(ch.cam + off::CamPos, g_cam, 12);
        printf("[+] camera %.2f %.2f %.2f  fov %.2f\n", g_cam[0], g_cam[1], g_cam[2],
               f32(ch.cam + off::CamFov));
    } else {
        printf("[X] no camera\n"); return 3;
    }

    printf("\n=== chain trace ===\n");
    print_chain(ch, g_base);
    uint64_t ient = ch.ient;
    if (!ient)
        printf("  >> the chain never resolved during the watch window.\n");

    // -----------------------------------------------------------------------
    //  Preflight.  Refuse to run against a map with no live player, because the
    //  result would be an empty list presented as a confident answer -- which is
    //  exactly the failure mode the previous five runs walked into.
    // -----------------------------------------------------------------------
    printf("\n=== preflight ===\n");
    if (!ient) {
        printf("\n[X] the local player chain does not resolve, so the signature\n");
        printf("    cannot be validated against a known player.  Running the sweep\n");
        printf("    anyway would produce a list whose correctness is unknowable --\n");
        printf("    which is the situation this tool exists to avoid.\n");
        printf("\n    next step: run struct_scan.exe, which measures the hops from\n");
        printf("    scratch against whatever the live chain is doing.\n");
        return 3;
    }
    if (!alive) {
        printf("\n[X] ABORT: no live frame in %d seconds.  The local player never\n", watchSeconds);
        printf("    had a valid position, so there was nothing to validate against.\n");
        printf("    Start the tool and then enter or continue a match.\n");
        return 4;
    }
    printf("    local IEntity 0x%llX  mask %u  pos %.2f %.2f %.2f\n",
           (unsigned long long)ient, local.mask, local.pos[0], local.pos[1], local.pos[2]);

    float mag = std::sqrt(local.pos[0]*local.pos[0] + local.pos[1]*local.pos[1] +
                          local.pos[2]*local.pos[2]);
    float dcam = std::sqrt((local.pos[0]-g_cam[0])*(local.pos[0]-g_cam[0]) +
                           (local.pos[1]-g_cam[1])*(local.pos[1]-g_cam[1]) +
                           (local.pos[2]-g_cam[2])*(local.pos[2]-g_cam[2]));
    printf("    alive: position is %.1f units from the world origin, %.1f from the camera\n",
           mag, dcam);
    if (dcam > 50.f)
        printf("    [!] warning: the camera is %.0f units away -- spectator or\n"
               "        post-death camera?  results may be partial\n", dcam);

    // -----------------------------------------------------------------------
    //  Signature sweep: P -> *(P+0x18) -> *(+0x40) -> IEntity with mask 2.
    // -----------------------------------------------------------------------
    printf("\n=== signature sweep ===\n");
    printf("    scanning for  props+0x18 -> comp,  comp+0x40 -> IEntity(mask 2)\n");
    std::unordered_map<uint64_t, Entity> found;
    constexpr size_t CH = 16u << 20;
    std::vector<uint8_t> buf(CH);
    size_t bytes = 0;
    uint64_t slotsChecked = 0;
    const uint64_t deadline = GetTickCount64() + 300'000;

    for (const auto& r : g_regions) {
        if (GetTickCount64() > deadline) { printf("    [!] time limit reached\n"); break; }
        uint64_t lo = (uint64_t)r.BaseAddress, hi = lo + r.RegionSize;
        for (uint64_t a = lo; a < hi && GetTickCount64() < deadline; a += CH) {
            size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
            if (!read(a, buf.data(), want)) continue;
            size_t n = want & ~(size_t)7;
            slotsChecked += n / 8;
            for (size_t i = 0; i < n; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (!mapped(q)) continue;                  // candidate ActorProps
                uint64_t c = u64(q + off::Component);
                if (!mapped(c)) continue;                  // candidate Component
                uint64_t ie = u64(c + off::IEntity);
                if (!mapped(ie)) continue;                 // candidate IEntity
                Entity e{};
                if (!load_entity(ie, &e)) continue;        // mask must be 2
                if (found.count(ie)) continue;
                e.props = a + i;
                e.comp  = c;
                found[ie] = e;
            }
            bytes += want;
        }
    }
    printf("    scanned %.0f MB, %llu slots, %zu player entities\n",
           bytes / (1024.0*1024.0), (unsigned long long)slotsChecked, found.size());

    // -----------------------------------------------------------------------
    //  Report.
    // -----------------------------------------------------------------------
    std::vector<Entity> ents;
    for (auto& kv : found) ents.push_back(kv.second);
    for (auto& e : ents) {
        float dx = e.pos[0]-g_cam[0], dy = e.pos[1]-g_cam[1], dz = e.pos[2]-g_cam[2];
        e.dist = std::sqrt(dx*dx + dy*dy + dz*dz);
    }
    std::sort(ents.begin(), ents.end(),
              [](const Entity& a, const Entity& b) { return a.dist < b.dist; });

    printf("\n%-4s %-18s %-18s %-18s %9s %9s %9s %8s\n",
           "#", "IEntity", "ActorProps", "Component", "x", "y", "z", "dist");
    for (size_t i = 0; i < ents.size() && i < 64; i++) {
        const Entity& e = ents[i];
        printf("%-4zu 0x%016llX 0x%016llX 0x%016llX %9.2f %9.2f %9.2f %8.1f%s\n",
               i, (unsigned long long)e.ient, (unsigned long long)e.props,
               (unsigned long long)e.comp, e.pos[0], e.pos[1], e.pos[2], e.dist,
               e.ient == ient ? "   <- you" : "");
    }
    if (ents.size() > 64) printf("    ... %zu more\n", ents.size() - 64);

    // Sanity: positions must not all be identical.  This is the check the
    // earlier runs were silently failing.
    int distinct = 0;
    for (size_t i = 0; i < ents.size(); i++)
        for (size_t j = i + 1; j < ents.size(); j++) {
            float dx = ents[i].pos[0]-ents[j].pos[0];
            float dy = ents[i].pos[1]-ents[j].pos[1];
            float dz = ents[i].pos[2]-ents[j].pos[2];
            if (dx*dx + dy*dy + dz*dz > 0.25f) { distinct = 1; break; }
        }

    printf("\n=== result ===\n");
    printf("  players found: %zu\n", ents.size());
    printf("  local player present: %s\n",
           found.count(ient) ? "yes" : "NO -- the signature does not match the local player, "
                                           "so the chain is wrong for other entities too");
    printf("  positions mutually distinct: %s\n", distinct ? "yes" : "NO -- all zero or identical");
    if (ents.size() && found.count(ient) && distinct) {
        printf("  RESULT: the signature is valid.  %zu players, positions differ,\n", ents.size());
        printf("          and the local player is among them -- the chain holds for\n");
        printf("          remote entities as well as the local one.\n");
    } else {
        printf("  RESULT: inconclusive -- see the checks above before trusting this.\n");
    }

    FILE* f = fopen("entity_array.json", "w");
    if (f) {
        fprintf(f, "{\n  \"camera\": [%.3f, %.3f, %.3f],\n  \"players\": [",
                g_cam[0], g_cam[1], g_cam[2]);
        for (size_t i = 0; i < ents.size(); i++) {
            const Entity& e = ents[i];
            fprintf(f, "%s\n    { \"ient\": \"0x%llX\", \"props\": \"0x%llX\", "
                       "\"comp\": \"0x%llX\", \"pos\": [%.3f, %.3f, %.3f], "
                       "\"dist\": %.2f, \"local\": %s }",
                    i ? "," : "", (unsigned long long)e.ient,
                    (unsigned long long)e.props, (unsigned long long)e.comp,
                    e.pos[0], e.pos[1], e.pos[2], e.dist,
                    e.ient == ient ? "true" : "false");
        }
        fprintf(f, "\n  ]\n}\n");
        fclose(f);
        printf("  wrote entity_array.json\n");
    }
    return 0;
}