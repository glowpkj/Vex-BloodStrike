// -----------------------------------------------------------------------------
//  entity_list.cpp  --  find the container holding every player.
//
//  Everything up to the local player is measured and verified (see
//  BLOODSTRIKE_ANALYSIS.md section 0):
//
//      base + 0x80D5980 -> ClientEngine -> +0x58 -> IGameplay
//        -> +0x58 -> ClientPlayer -> +0xDD8 -> LocalPlayer
//        -> +0x18 -> ActorInstance -> +0x48 -> ActorProps
//        -> +0x18 -> Component -> +0x40 -> IEntity
//        -> +0x1C0 position,  +0x3E0 EntityMask (2 = player)
//
//  That reaches one player.  An ESP needs all of them, and the published
//  container forms do not compose on this build:
//
//    * a circular linked list whose nodes carry the actor at +0x18 -- the head
//      resolves, but its first node has no forward pointer, so it is not that
//    * a flat pointer array -- no array was found with a plausible live ratio
//
//  So this searches for the container by what it must contain rather than by
//  what it is called.  Three shapes are tested, in the order the evidence
//  supports:
//
//    1. a contiguous array of pointers, each resolving through the known chain
//       to an IEntity with a sane EntityMask and a finite position
//    2. the same, but allowing one indirection hop, because the array may hold
//       handles rather than instances
//    3. a linked structure, trying every plausible next-pointer offset instead
//       of assuming +0x0
//
//  The discriminator is not "some members look right".  It is: a real entity
//  list yields MULTIPLE members whose EntityMask marks them as players, whose
//  positions are mutually distinct, and which are spread across the map rather
//  than stacked on one point.  Random pointers satisfy none of that.
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
#include <unordered_set>

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

    constexpr uint64_t ActorProps   = 0x48;   // measured, not published
    constexpr uint64_t Component    = 0x18;
    constexpr uint64_t IEntity      = 0x40;
    constexpr uint64_t EntPos       = 0x1C0;
    constexpr uint64_t EntMask      = 0x3E0;
}

namespace {

HANDLE   g_h;
uint64_t g_base;
float    g_cam[3];
uint64_t g_localIent = 0;

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

/// Resolve an IEntity from something that might be an ActorInstance, a
/// Component, or an IEntity already.  Returns 0 if none of those chains work.
uint64_t resolve_ient(uint64_t p) {
    if (!mapped(p)) return 0;
    // already an IEntity?
    uint32_t mask = u32(p + off::EntMask);
    if (mask >= 1 && mask <= 64) {
        float q[3];
        if (read(p + off::EntPos, q, 12) && std::isfinite(q[0]) && std::isfinite(q[1]) &&
            std::isfinite(q[2]) && (std::fabs(q[0]) < 1e5f) && (std::fabs(q[2]) < 1e5f))
            return p;
    }
    // ActorInstance -> ActorProps -> Component -> IEntity
    uint64_t props = u64(p + off::ActorProps);
    if (mapped(props)) {
        uint64_t comp = u64(props + off::Component);
        if (mapped(comp)) {
            uint64_t ient = u64(comp + off::IEntity);
            uint32_t m = u32(ient + off::EntMask);
            if (mapped(ient) && m >= 1 && m <= 64) return ient;
        }
    }
    return 0;
}

struct Entity { uint64_t ient; float pos[3]; uint32_t mask; };

bool load_entity(uint64_t ient, Entity* e) {
    if (!mapped(ient)) return false;
    e->ient = ient;
    e->mask = u32(ient + off::EntMask);
    if (!read(ient + off::EntPos, e->pos, 12)) return false;
    for (int i = 0; i < 3; i++) if (!std::isfinite(e->pos[i])) return false;
    return std::fabs(e->pos[0]) < 1e5f && std::fabs(e->pos[1]) < 1e5f && std::fabs(e->pos[2]) < 1e5f;
}

} // namespace

int main() {
    printf("entity_list -- locate the container holding every player\n");
    printf("===========================================================\n");

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

    uint64_t dataLo = 0, dataHi = 0, textLo = 0, textHi = 0;
    {
        wchar_t path[MAX_PATH]{}; DWORD n = MAX_PATH;
        QueryFullProcessImageNameW(g_h, 0, path, &n);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::vector<uint8_t> head(1u << 20);
        DWORD got = 0;
        if (f != INVALID_HANDLE_VALUE) { ReadFile(f, head.data(), (DWORD)head.size(), &got, nullptr);
                                         CloseHandle(f); }
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
    (void)textLo; (void)textHi;

    uint64_t engine = u64(g_base + off::ClientEngine);
    uint64_t gp = mapped(engine) ? u64(engine + off::IGameplay) : 0;
    uint64_t pl = mapped(gp) ? u64(gp + off::ClientPlayer) : 0;
    uint64_t cam = mapped(pl) ? u64(pl + off::Camera) : 0;
    if (!mapped(cam)) { printf("[X] camera chain failed\n"); return 3; }
    read(cam + off::CamPos, g_cam, 12);
    uint64_t loc = mapped(pl) ? u64(pl + off::LocalPlayer) : 0;
    uint64_t inst = mapped(loc) ? u64(loc + off::ActorInst) : 0;
    g_localIent = mapped(inst) ? resolve_ient(inst) : 0;
    printf("[+] camera   %.2f %.2f %.2f  fov %.2f\n", g_cam[0], g_cam[1], g_cam[2],
           f32(cam + off::CamFov));
    printf("[+] local IEntity 0x%llX  %s\n", (unsigned long long)g_localIent,
           g_localIent ? "resolved" : "FAILED");

    // ======================================================================
    //  Shape 1 and 2 -- contiguous pointer arrays.
    //
    //  Slide a window over every writable region and, at each 8-byte slot,
    //  measure how many CONSECUTIVE entries resolve to player entities.  A real
    //  entity array produces a long run; anything else produces scattered hits,
    //  which the "consecutive" requirement throws away.
    // ======================================================================
    printf("\n=== searching contiguous pointer arrays ===\n");
    struct Best { uint64_t va; int run; int players; float spread; int total; };
    std::vector<Best> best;

    constexpr int kMinRun = 8;          // at least eight consecutive actors
    constexpr size_t CH = 8u << 20;
    std::vector<uint8_t> buf(CH);
    const uint64_t deadline = GetTickCount64() + 240'000;

    auto sweep = [&](uint64_t lo, uint64_t hi) {
        for (uint64_t a = lo; a < hi && GetTickCount64() < deadline; a += CH) {
            size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
            if (!read(a, buf.data(), want)) continue;

            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (!mapped(q)) continue;

                // measure the run starting here
                int run = 0, players = 0, total = 0;
                std::vector<Entity> ents;
                for (size_t j = i; j + 8 <= want; j += 8) {
                    uint64_t v;
                    memcpy(&v, buf.data() + j, 8);
                    if (v == 0) break;
                    uint64_t ie = resolve_ient(v);
                    if (!ie) break;
                    Entity e{};
                    if (!load_entity(ie, &e)) break;
                    run++;
                    if (e.mask == 2 || e.mask == 1) { players++; ents.push_back(e); }
                    if (run > 256) break;
                }
                if (run < kMinRun) { i += 0; continue; }

                // The discriminator has three parts and all three are required.
                // An earlier version tested only the first and reported
                // "run 102, players 102" -- which turned out to be one IEntity
                // repeated 102 times at the origin.  A real player list has:
                //
                //   DISTINCT  pointers, not the same one repeated
                //   NON-ZERO  positions, not a default-initialised slot
                //   SPREAD    positions that differ, i.e. players in different
                //             places rather than stacked on one point
                std::unordered_set<uint64_t> distinct;
                for (const auto& e : ents) distinct.insert(e.ient);
                int nonzero = 0;
                for (const auto& e : ents)
                    if (std::fabs(e.pos[0]) > 0.5f || std::fabs(e.pos[2]) > 0.5f) nonzero++;

                float spread = 0.f;
                if (players >= 2) {
                    float cx = 0, cy = 0, cz = 0;
                    for (const auto& e : ents) { cx += e.pos[0]; cy += e.pos[1]; cz += e.pos[2]; }
                    cx /= ents.size(); cy /= ents.size(); cz /= ents.size();
                    for (const auto& e : ents) {
                        float dx = e.pos[0]-cx, dy = e.pos[1]-cy, dz = e.pos[2]-cz;
                        spread += dx*dx + dy*dy + dz*dz;
                    }
                    spread = std::sqrt(spread / ents.size());
                }
                int uniq = (int)distinct.size();
                if (uniq < 3 || nonzero < 3 || spread < 1.f) { i += 0; continue; }

                best.push_back({ a + i, run, players, spread, uniq });
            }
        }
    };
    sweep(dataLo, dataHi);
    for (const auto& r : g_regions)
        if (r.Protect & PAGE_READWRITE)
            sweep((uint64_t)r.BaseAddress, (uint64_t)r.BaseAddress + r.RegionSize);

    std::sort(best.begin(), best.end(), [](const Best& a, const Best& b) {
        if (a.players != b.players) return a.players > b.players;
        if (a.spread != b.spread) return a.spread > b.spread;
        return a.run > b.run;
    });

    printf("    arrays with >= %d consecutive resolvable members: %zu\n\n", kMinRun, best.size());
    int shown = 0;
    for (const auto& b : best) {
        if (shown++ >= 8) break;
        printf("    0x%llX  run %d  players %d  spread %.1f\n",
               (unsigned long long)b.va, b.run, b.players, b.spread);
        int k = 0;
        for (size_t j = 0; j < 24 && k < 10; j++) {
            uint64_t v = u64(b.va + j * 8);
            if (!v) { printf("        [%2d]  -\n", k); continue; }
            uint64_t ie = resolve_ient(v);
            Entity e{};
            if (!ie || !load_entity(ie, &e)) { printf("        [%2d]  unresolved\n", k); k++; continue; }
            float d = std::sqrt((e.pos[0]-g_cam[0])*(e.pos[0]-g_cam[0]) +
                                (e.pos[1]-g_cam[1])*(e.pos[1]-g_cam[1]) +
                                (e.pos[2]-g_cam[2])*(e.pos[2]-g_cam[2]));
            printf("        [%2d]  ient 0x%llX mask %u  pos %8.2f %8.2f %8.2f  d=%.1f%s\n",
                   k, (unsigned long long)e.ient, e.mask, e.pos[0], e.pos[1], e.pos[2], d,
                   (e.ient == g_localIent) ? "  <- you" : "");
            k++;
        }
        printf("\n");
    }

    // ======================================================================
    //  Shape 3 -- linked structure, every plausible next-pointer offset.
    //
    //  Starting from the EntityList global, try each candidate offset for both
    //  "the head" and "the next node", and see which combination produces a
    //  cycle that visits more than one player.
    // ======================================================================
    printf("=== linked structures from the EntityList global ===\n");
    printf("    global value 0x%llX\n", (unsigned long long)u64(g_base + off::EntityList));
    const uint64_t nextHops[] = { 0x0, 0x8, 0x10, 0x18, 0x20, 0x28, 0x30 };
    const uint64_t headHops[] = { 0x0, 0x8, 0x10, 0x18, 0x20 };

    for (uint64_t hh : headHops) {
        uint64_t head = u64(g_base + off::EntityList + hh);
        if (!mapped(head)) continue;
        for (uint64_t nh : nextHops) {
            int visited = 0, players = 0;
            std::unordered_map<uint64_t, int> seen;
            uint64_t cur = head;
            while (mapped(cur) && visited < 512) {
                if (seen[cur]++ > 1) break;
                uint64_t ie = resolve_ient(cur);
                if (ie) {
                    Entity e{};
                    if (load_entity(ie, &e) && (e.mask == 1 || e.mask == 2)) players++;
                }
                uint64_t next = u64(cur + nh);
                if (next == cur || !mapped(next)) break;
                cur = next;
                if (cur == head) break;
            }
            if (players >= 2)
                printf("    head+%02X -> next+%02X : %d nodes, %d players\n",
                       (unsigned)hh, (unsigned)nh, visited, players);
        }
    }
    printf("    (nothing printed above means no hop combination walks a cycle)\n");

    printf("\n=== result ===\n");
    if (!best.empty() && best.front().players >= 2)
        printf("  ENTITY LIST FOUND: 0x%llX, %d players, spread %.1f\n",
               (unsigned long long)best.front().va, best.front().players,
               best.front().spread);
    else
        printf("  no entity container found.  The players are reachable from the\n");
        printf("  local player but the container that enumerates them is not in\n");
        printf("  writable memory in a shape this search recognises.\n");
    return 0;
}
