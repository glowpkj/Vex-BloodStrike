// -----------------------------------------------------------------------------
//  find_refs.cpp  --  reverse reference search anchored on the local player.
//
//  Four attempts to find the player container by sweeping memory all failed in
//  the same way: the sweep returns candidate structures, and every candidate
//  test had to be tightened until it rejected the false positives.  A loose test
//  accepts heap noise; a strict one rejects the real list too, because I do not
//  know the container's shape and so cannot tell noise from a valid array of
//  handles.
//
//  This file removes the guessing entirely by inverting the search direction.
//
//  The local player is already resolved and verified:
//
//      IEntity     0x219BD4ED850   EntityMask == 2
//      position    (-17.18, -0.07, -7.63)   1.4 units from the camera
//
//  Those are facts, not guesses.  So instead of asking "which memory looks like
//  a list of players", this asks "where in memory does this exact address
//  appear".  Every 8-byte slot in the process is compared against the four
//  known-good objects on the local player chain -- the ActorInstance, its
//  ActorProps, the Component and the IEntity.  Whatever container enumerates
//  players must hold at least one of them, and the hit address is an exact
//  answer rather than a score.
//
//  From each hit the container is then measured rather than assumed: the run of
//  adjacent slots that resolve to distinct entities is walked in both
//  directions, and the bytes immediately before the run are dumped so a header
//  (count, capacity, lock, owner pointer) can be recognised by eye.
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
    constexpr uint64_t IGameplay    = 0x58;
    constexpr uint64_t ClientPlayer = 0x58;
    constexpr uint64_t Camera       = 0xD88;
    constexpr uint64_t LocalPlayer  = 0xDD8;
    constexpr uint64_t CamPos       = 0x7C;
    constexpr uint64_t CamFov       = 0x454;
    constexpr uint64_t ActorInst    = 0x18;
    constexpr uint64_t ActorProps   = 0x48;
    constexpr uint64_t Component    = 0x18;
    constexpr uint64_t IEntity      = 0x40;
    constexpr uint64_t EntPos       = 0x1C0;
    constexpr uint64_t EntMask      = 0x3E0;
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

struct Entity { uint64_t ient; float pos[3]; uint32_t mask; };

bool load_entity(uint64_t ient, Entity* e) {
    if (!mapped(ient)) return false;
    e->ient = ient;
    e->mask = u32(ient + off::EntMask);
    if (!read(ient + off::EntPos, e->pos, 12)) return false;
    for (int i = 0; i < 3; i++) if (!std::isfinite(e->pos[i])) return false;
    return std::fabs(e->pos[0]) < 1e5f && std::fabs(e->pos[1]) < 1e5f &&
           std::fabs(e->pos[2]) < 1e5f;
}

/// Map a candidate pointer onto an IEntity.  The chain found by struct_scan is
/// ActorInstance -> +0x48 -> props -> +0x18 -> comp -> +0x40 -> IEntity.  A
/// container may hold any of the three intermediate objects rather than the leaf,
/// so all three entry points are accepted, plus the leaf itself.
uint64_t resolve_ient(uint64_t p) {
    if (!mapped(p)) return 0;
    Entity e{};
    if (load_entity(p, &e) && e.mask >= 1 && e.mask <= 64) return p;   // already IEntity

    uint64_t props = u64(p + off::ActorProps);                        // p is an ActorInstance
    if (!mapped(props)) props = p;                                     // p is already props
    uint64_t comp = u64(props + off::Component);
    if (!mapped(comp)) return 0;
    uint64_t ie = u64(comp + off::IEntity);
    if (load_entity(ie, &e) && e.mask >= 1 && e.mask <= 64) return ie;
    return 0;
}

float dist_from_cam(const float* p) {
    float dx = p[0]-g_cam[0], dy = p[1]-g_cam[1], dz = p[2]-g_cam[2];
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

/// A contiguous span of slots that each resolve to a distinct entity.
struct Run {
    uint64_t begin = 0, end = 0;     // half-open, 8-byte stride
    int      count = 0, players = 0;
    std::vector<Entity> ents;
    uint64_t localSlot = 0;
};

/// Walk outwards from `at` in 8-byte steps collecting resolvable slots.
Run walk_run(uint64_t at) {
    Run r;
    // forward
    {
        uint64_t a = at;
        std::unordered_set<uint64_t> seen;
        for (int i = 0; i < 512; i++, a += 8) {
            uint64_t q = u64(a);
            if (!q) break;
            uint64_t ie = resolve_ient(q);
            Entity e{};
            if (!ie || !load_entity(ie, &e)) break;
            r.ents.push_back(e);
            if (e.mask == 2) r.players++;
            if (!seen.insert(e.ient).second && r.ents.size() > 4) break;
        }
        r.begin = at;
    }
    // backward: re-walk in front of the anchor and prepend
    {
        std::vector<Entity> back;
        std::unordered_set<uint64_t> seen;
        for (uint64_t a = at; a + 8 > at; a -= 8) {
            if (a == 0) break;
            uint64_t q = u64(a);
            if (!q) break;
            uint64_t ie = resolve_ient(q);
            Entity e{};
            if (!ie || !load_entity(ie, &e)) break;
            back.push_back(e);
            if (!seen.insert(e.ient).second && back.size() > 4) break;
            if (back.size() >= 512) break;
        }
        std::reverse(back.begin(), back.end());
        r.begin = at - (uint64_t)back.size() * 8;
        r.ents.insert(r.ents.begin(), back.begin(), back.end());
        r.players = 0;
        for (const auto& e : r.ents) if (e.mask == 2) r.players++;
    }
    r.count = (int)r.ents.size();
    r.end   = r.begin + (uint64_t)r.count * 8;
    return r;
}

struct Hit { uint64_t where; int which; };   // which: 0 inst, 1 props, 2 comp, 3 ient

} // namespace

int main() {
    printf("find_refs -- locate the player container by reverse reference\n");
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

    uint64_t engine = u64(g_base + off::ClientEngine);
    uint64_t gp = mapped(engine) ? u64(engine + off::IGameplay) : 0;
    uint64_t pl = mapped(gp) ? u64(gp + off::ClientPlayer) : 0;
    uint64_t cam = mapped(pl) ? u64(pl + off::Camera) : 0;
    if (!mapped(cam)) { printf("[X] camera chain failed\n"); return 3; }
    read(cam + off::CamPos, g_cam, 12);
    uint64_t loc  = mapped(pl) ? u64(pl + off::LocalPlayer) : 0;
    uint64_t inst = mapped(loc) ? u64(loc + off::ActorInst) : 0;
    uint64_t props = mapped(inst) ? u64(inst + off::ActorProps) : 0;
    uint64_t comp  = mapped(props) ? u64(props + off::Component) : 0;
    uint64_t ient  = mapped(comp) ? u64(comp + off::IEntity) : 0;

    printf("[+] pid %u  base 0x%llX  %zu readable regions (%.0f MB)\n", pid,
           (unsigned long long)g_base, g_regions.size(),
           [&]{ double t = 0; for (auto& r : g_regions) t += (double)r.RegionSize;
                return t / (1024.0*1024.0); }());
    printf("[+] camera %.2f %.2f %.2f  fov %.2f\n", g_cam[0], g_cam[1], g_cam[2],
           f32(cam + off::CamFov));
    printf("[+] local chain:\n");
    printf("      LocalPlayer  0x%llX\n", (unsigned long long)loc);
    printf("      ActorInstance 0x%llX\n", (unsigned long long)inst);
    printf("      ActorProps   0x%llX\n", (unsigned long long)props);
    printf("      Component    0x%llX\n", (unsigned long long)comp);
    printf("      IEntity      0x%llX\n", (unsigned long long)ient);
    if (!ient) { printf("[X] local chain incomplete\n"); return 3; }
    {
        Entity e{}; load_entity(ient, &e);
        printf("      mask %u  pos %.2f %.2f %.2f  (%.1f units from camera)\n",
               e.mask, e.pos[0], e.pos[1], e.pos[2], dist_from_cam(e.pos));
    }

    const uint64_t targets[4] = { inst, props, comp, ient };
    const char*    names[4]  = { "ActorInstance", "ActorProps", "Component", "IEntity" };

    // -----------------------------------------------------------------------
    //  Sweep every readable 8-byte slot for the four known objects.
    // -----------------------------------------------------------------------
    printf("\n=== scanning for pointers to the local player ===\n");
    std::vector<Hit> hits;
    constexpr size_t CH = 16u << 20;
    std::vector<uint8_t> buf(CH);
    size_t bytes = 0;
    const uint64_t deadline = GetTickCount64() + 300'000;

    for (const auto& r : g_regions) {
        if (GetTickCount64() > deadline) { printf("    [!] time limit at %zu regions\n", hits.size()); break; }
        uint64_t lo = (uint64_t)r.BaseAddress, hi = lo + r.RegionSize;
        for (uint64_t a = lo; a < hi && GetTickCount64() < deadline; a += CH) {
            size_t want = (size_t)((hi - a) < CH ? (hi - a) : CH);
            if (!read(a, buf.data(), want)) continue;
            size_t n = want & ~(size_t)7;
            for (size_t i = 0; i < n; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (q != inst && q != props && q != comp && q != ient) continue;
                int w = (q == inst) ? 0 : (q == props) ? 1 : (q == comp) ? 2 : 3;
                hits.push_back({ a + i, w });
            }
            bytes += want;
        }
    }
    printf("    scanned %.0f MB, found %zu references\n", bytes / (1024.0*1024.0), hits.size());

    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.where != b.where) return a.where < b.where;
        return a.which < b.which; });
    hits.erase(std::unique(hits.begin(), hits.end(),
                           [](const Hit& a, const Hit& b) {
                               return a.where == b.where && a.which == b.which; }),
               hits.end());

    int cnt[4] = { 0, 0, 0, 0 };
    for (const auto& h : hits) cnt[h.which]++;
    for (int i = 0; i < 4; i++)
        printf("      %-14s %d\n", names[i], cnt[i]);

    // -----------------------------------------------------------------------
    //  Describe the neighbourhood of every reference.
    //
    //  The 8-byte-stride walk found nothing, which by itself is informative: it
    //  means the container is not a flat vector of 8-byte pointers.  It could be
    //  a vector of nodes with a wider stride (the published layout puts the actor
    //  at node+0x18, so a node is at least 0x20 bytes), a tree, or a structure
    //  that stores the entity as a member rather than in a slot.  Only the bytes
    //  around each reference can tell those apart, so print them -- annotated,
    //  with every pointer classified rather than left as a number.
    // -----------------------------------------------------------------------
    printf("\n=== neighbourhood of every reference ===\n");
    auto regionOf = [&](uint64_t va) -> const MEMORY_BASIC_INFORMATION* {
        size_t lo = 0, hi = g_regions.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            uint64_t a = (uint64_t)g_regions[mid].BaseAddress;
            uint64_t b = a + g_regions[mid].RegionSize;
            if (va < a) hi = mid;
            else if (va >= b) lo = mid + 1;
            else return &g_regions[mid];
        }
        return nullptr;
    };

    for (const auto& h : hits) {
        const MEMORY_BASIC_INFORMATION* ri = regionOf(h.where);
        bool rw = ri && (ri->Protect & PAGE_READWRITE);
        uint64_t baseRel = 0;
        bool inExe = false;
        if (h.where >= g_base && h.where < g_base + 0x20000000) {
            baseRel = h.where - g_base; inExe = true;
        }
        printf("\n  reference at 0x%llX -> %s   [%s%s]\n",
               (unsigned long long)h.where, names[h.which],
               rw ? "writable" : "read-only",
               inExe ? (baseRel >= 0x1000 && baseRel < 0x60000000
                        ? ", in exe image" : ", in exe space")
                     : ", heap");
        uint64_t lo = h.where >= 0x40 ? h.where - 0x40 : h.where;
        for (uint64_t a = lo; a < h.where + 0x48; a += 8) {
            uint64_t v = u64(a);
            char note[128] = "";
            if (v) {
                if (v == inst)  snprintf(note, sizeof note, "  ActorInstance (local)");
                else if (v == props) snprintf(note, sizeof note, "  ActorProps (local)");
                else if (v == comp)  snprintf(note, sizeof note, "  Component (local)");
                else if (v == ient) snprintf(note, sizeof note, "  IEntity (local)");
                else if (v == loc)  snprintf(note, sizeof note, "  LocalPlayer (local)");
                else if (mapped(v)) {
                    Entity e{};
                    if (load_entity(v, &e) && e.mask >= 1 && e.mask <= 64)
                        snprintf(note, sizeof note, "  -> IEntity? mask %u pos %.2f %.2f %.2f",
                                 e.mask, e.pos[0], e.pos[1], e.pos[2]);
                    else {
                        uint64_t ie = resolve_ient(v);
                        if (ie) {
                            Entity e2{};
                            load_entity(ie, &e2);
                            snprintf(note, sizeof note,
                                     "  -> chain to IEntity 0x%llX mask %u pos %.2f %.2f %.2f",
                                     (unsigned long long)ie, e2.mask,
                                     e2.pos[0], e2.pos[1], e2.pos[2]);
                        } else {
                            uint64_t ip = u64(v + off::ActorProps);
                            snprintf(note, sizeof note, "  -> mapped%s",
                                     mapped(ip) ? ", +0x48 points into heap" : "");
                        }
                    }
                } else if (v < 0x100000000ull) {
                    snprintf(note, sizeof note, "  = %llu", (unsigned long long)v);
                } else snprintf(note, sizeof note, "  (unmapped)");
            } else note[0] = 0;
            printf("      %s 0x%llX  0x%016llX%s\n", a == h.where ? "->" : "  ",
                   (unsigned long long)a, (unsigned long long)v, note);
        }
    }

    // -----------------------------------------------------------------------
    //  Same measurement, but with the stride left free.  A node-based container
    //  breaks an 8-byte walk on the first slot and looks like nothing at all.
    // -----------------------------------------------------------------------
    printf("\n=== runs with a free stride ===\n");
    struct Cand { uint64_t begin; int count; int players; int distinct; int stride;
                  uint64_t hit; int which; std::vector<Entity> ents; };
    std::vector<Cand> cands;
    const int strides[] = { 8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60 };

    for (const auto& h : hits) {
        for (int st : strides) {
            // walk forward only; the run must contain the reference itself
            std::vector<Entity> ents;
            std::unordered_set<uint64_t> distinct;
            int players = 0;
            for (int i = 0; i < 128; i++) {
                uint64_t q = u64(h.where + (uint64_t)i * (uint64_t)st);
                if (!q) break;
                uint64_t ie = resolve_ient(q);
                Entity e{};
                if (!ie || !load_entity(ie, &e)) break;
                ents.push_back(e);
                distinct.insert(e.ient);
                if (e.mask == 2) players++;
            }
            if (ents.size() < 2 || distinct.size() < 2) continue;
            cands.push_back({ h.where, (int)ents.size(), players, (int)distinct.size(),
                              st, h.where, h.which, ents });
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.distinct != b.distinct) return a.distinct > b.distinct;
        return a.count > b.count; });
    printf("    %zu candidate runs\n", cands.size());
    int shown = 0;
    for (const auto& c : cands) {
        if (shown++ >= 6) { printf("    ... %zu more\n", cands.size() - 6); break; }
        printf("\n  run at 0x%llX  stride %d  count %d  distinct %d  players %d  "
               "(ref -> %s)\n", (unsigned long long)c.hit, c.stride, c.count,
               c.distinct, c.players, names[c.which]);
        for (int i = 0; i < c.count && i < 24; i++) {
            const Entity& e = c.ents[i];
            printf("        [%2d] ient 0x%llX mask %2u  pos %9.2f %9.2f %9.2f  d=%6.1f%s\n",
                   i, (unsigned long long)e.ient, e.mask, e.pos[0], e.pos[1], e.pos[2],
                   dist_from_cam(e.pos), e.ient == ient ? "   <- local" : "");
        }
    }

    std::vector<Cand> merged = cands;

    for (const auto& c : merged) {
        std::unordered_set<uint64_t> distinct;
        int players = 0;
        for (const auto& e : c.ents) { distinct.insert(e.ient); if (e.mask == 2) players++; }
        printf("\n  merged run at 0x%llX  count %d  distinct %zu  players(mask==2) %d\n",
               (unsigned long long)c.begin, c.count, distinct.size(), players);
        printf("      first reference at 0x%llX -> %s\n",
               (unsigned long long)c.hit, names[c.which]);
        for (int i = 0; i < c.count && i < 32; i++) {
            Entity e = c.ents[i];
            printf("        [%2d] ient 0x%llX mask %2u  pos %9.2f %9.2f %9.2f  d=%6.1f%s\n",
                   i, (unsigned long long)e.ient, e.mask, e.pos[0], e.pos[1], e.pos[2],
                   dist_from_cam(e.pos), e.ient == ient ? "   <- local player" : "");
        }
        if (c.count > 32) printf("        ... %d more\n", c.count - 32);

        // dump the 0x80 bytes before the run so a header is visible
        printf("      header before the run:\n");
        uint64_t hb = c.begin >= 0x80 ? c.begin - 0x80 : c.begin;
        for (uint64_t a = hb; a < c.begin; a += 8) {
            uint64_t v = u64(a);
            char note[96] = "";
            if (v && mapped(v)) {
                Entity e{};
                if (load_entity(v, &e) && e.mask >= 1 && e.mask <= 64)
                    snprintf(note, sizeof note, "  -> IEntity? mask %u pos %.2f %.2f %.2f",
                             e.mask, e.pos[0], e.pos[1], e.pos[2]);
                else
                    snprintf(note, sizeof note, "  -> mapped");
            } else if (v && v < 0x100000) {
                snprintf(note, sizeof note, "  = %llu", (unsigned long long)v);
            }
            if (a + 8 == c.begin) snprintf(note, sizeof note, "  <== first slot of run");
            printf("        0x%llX  0x%016llX%s\n", (unsigned long long)a,
                   (unsigned long long)v, note);
        }
    }

    // -----------------------------------------------------------------------
    //  If a run contains the local player and at least one other distinct player
    //  with a different position, it is the list.  Save it.
    // -----------------------------------------------------------------------
    Run best{};
    bool haveBest = false;
    const Cand* bestCand = nullptr;
    for (const auto& c : merged) {
        bool hasLocal = false;
        std::unordered_set<uint64_t> distinct;
        int players = 0;
        for (const auto& e : c.ents) {
            if (e.ient == ient) hasLocal = true;
            distinct.insert(e.ient);
            if (e.mask == 2) players++;
        }
        if (!hasLocal) continue;
        if (distinct.size() < 2 || players < 2) continue;
        if (!haveBest || c.count > best.count) {
            best.count = c.count; best.ents = c.ents; best.begin = c.begin;
            haveBest = true; bestCand = &c;
        }
    }

    printf("\n=== result ===\n");
    if (!haveBest) {
        printf("  no container found.  The local player is not stored inside any\n");
        printf("  run of adjacent resolvable slots, so the enumeration lives in a\n");
        printf("  structure whose members are not directly resolvable by this chain\n");
        printf("  (handles, ids, or a different component layout).\n");
        return 0;
    }

    printf("  ENTITY LIST: 0x%llX  %d slots\n", (unsigned long long)best.begin, best.count);
    FILE* f = fopen("entity_array.json", "w");
    if (f) {
        fprintf(f, "{\n  \"array_va\": \"0x%llX\",\n  \"slots\": %d,\n  \"entries\": [",
                (unsigned long long)best.begin, best.count);
        for (int i = 0; i < best.count; i++) {
            Entity e = best.ents[i];
            fprintf(f, "%s\n    { \"ient\": \"0x%llX\", \"mask\": %u, "
                       "\"pos\": [%.3f, %.3f, %.3f] }",
                    i ? "," : "", (unsigned long long)e.ient, e.mask,
                    e.pos[0], e.pos[1], e.pos[2]);
        }
        fprintf(f, "\n  ]\n}\n");
        fclose(f);
        printf("  wrote entity_array.json\n");
    }
    return 0;
}