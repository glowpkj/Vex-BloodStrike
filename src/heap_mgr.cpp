// -----------------------------------------------------------------------------
//  heap_mgr.cpp  --  reach the player list through EntityHeapManager.
//
//  Every published way of *finding* the entity container by scanning has
//  produced false positives: a heap with five million pointers will always
//  yield an "array of players" if the acceptance test is loose, and tightening
//  it costs an iteration per attempt.
//
//  The dump supplied with this project names a different route entirely:
//
//      EntityHeapManager = 0x7FCC540
//      +0x320 -> VectorBase
//      +0x328 -> VectorEnd
//
//  That is a bucket-based allocator, not a player list, but it is a *known
//  address* that leads into the entity allocation machinery.  Walking it
//  reaches the manager object, and from the manager the container is one or two
//  hops away -- which is a directed search instead of a sweep.
//
//  This file does exactly that, and reports every hop so a dead end is visible
//  rather than guessed around.
//
//  Note on the earlier runs: 0x7FCC540 read back as the ASCII "llisionR" on this
//  build, so that offset is stale like the others.  The tool therefore treats
//  it as a *hint*: it scans .data for any slot whose value is a live object
//  leading to a plausible bucket manager, rather than trusting the number.
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

    constexpr uint64_t HeapMgrHint  = 0x7FCC540;
    constexpr uint64_t BucketBase   = 0x320;
    constexpr uint64_t BucketEnd    = 0x328;
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

struct Entity { uint64_t ient; float pos[3]; uint32_t mask; };

bool load_entity(uint64_t ient, Entity* e) {
    if (!mapped(ient)) return false;
    e->ient = ient;
    e->mask = u32(ient + off::EntMask);
    if (!read(ient + off::EntPos, e->pos, 12)) return false;
    for (int i = 0; i < 3; i++) if (!std::isfinite(e->pos[i])) return false;
    return std::fabs(e->pos[0]) < 1e5f && std::fabs(e->pos[1]) < 1e5f && std::fabs(e->pos[2]) < 1e5f;
}

uint64_t resolve_ient(uint64_t p) {
    if (!mapped(p)) return 0;
    // already an IEntity?
    Entity tmp{};
    if (load_entity(p, &tmp) && tmp.mask >= 1 && tmp.mask <= 64) return p;
    uint64_t props = u64(p + off::ActorProps);
    if (mapped(props)) {
        uint64_t comp = u64(props + off::Component);
        if (mapped(comp)) {
            uint64_t ie = u64(comp + off::IEntity);
            Entity t{};
            if (load_entity(ie, &t) && t.mask >= 1 && t.mask <= 64) return ie;
        }
    }
    return 0;
}

/// Walk an array and report what it actually contains.  The acceptance test is
/// deliberately strict, because a loose one is what produced three rounds of
/// false positives: require DISTINCT entities, several with mask == 2, and
/// positions that differ from each other.
struct ArrayVerdict {
    uint64_t va = 0;
    int slots = 0, resolvable = 0, players = 0, distinct = 0;
    float spread = 0.f;
    bool good = false;
};

ArrayVerdict probe_array(uint64_t begin, uint64_t end) {
    ArrayVerdict v;
    v.va = begin;
    if (end < begin) return v;
    uint64_t bytes = end - begin;
    if (bytes < 32 || bytes > (16ULL << 20)) return v;
    v.slots = (int)(bytes / 8);
    if (v.slots > 8192) return v;

    std::vector<Entity> ents;
    std::unordered_set<uint64_t> seen;
    for (int i = 0; i < v.slots; i++) {
        uint64_t q = u64(begin + uint64_t(i) * 8);
        if (!q) continue;
        uint64_t ie = resolve_ient(q);
        if (!ie) continue;
        Entity e{};
        if (!load_entity(ie, &e)) continue;
        v.resolvable++;
        if (e.mask == 2) { v.players++; ents.push_back(e); seen.insert(e.ient); }
    }
    v.distinct = (int)seen.size();
    if (ents.size() >= 2) {
        double cx = 0, cy = 0, cz = 0;
        for (const auto& e : ents) { cx += e.pos[0]; cy += e.pos[1]; cz += e.pos[2]; }
        cx /= ents.size(); cy /= ents.size(); cz /= ents.size();
        double s = 0;
        for (const auto& e : ents) {
            double dx = e.pos[0]-cx, dy = e.pos[1]-cy, dz = e.pos[2]-cz;
            s += dx*dx + dy*dy + dz*dz;
        }
        v.spread = (float)std::sqrt(s / ents.size());
    }
    v.good = (v.distinct >= 3 && v.players >= 3 && v.spread > 2.f);
    return v;
}

} // namespace

int main() {
    printf("heap_mgr -- reach the player list through the heap manager\n");
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

    uint64_t dataLo = 0, dataHi = 0;
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

    uint64_t engine = u64(g_base + off::ClientEngine);
    uint64_t gp = mapped(engine) ? u64(engine + off::IGameplay) : 0;
    uint64_t pl = mapped(gp) ? u64(gp + off::ClientPlayer) : 0;
    uint64_t cam = mapped(pl) ? u64(pl + off::Camera) : 0;
    if (!mapped(cam)) { printf("[X] camera chain failed\n"); return 3; }
    read(cam + off::CamPos, g_cam, 12);
    uint64_t loc = mapped(pl) ? u64(pl + off::LocalPlayer) : 0;
    uint64_t inst = mapped(loc) ? u64(loc + off::ActorInst) : 0;
    g_localIent = mapped(inst) ? resolve_ient(inst) : 0;
    printf("[+] camera %.2f %.2f %.2f  fov %.2f\n", g_cam[0], g_cam[1], g_cam[2],
           f32(cam + off::CamFov));
    printf("[+] local IEntity 0x%llX\n", (unsigned long long)g_localIent);

    // ---------------------------------------------------------------------
    //  Step 1 -- the hinted global, for the record.
    // ---------------------------------------------------------------------
    printf("\n=== the hint from the dump ===\n");
    {
        uint64_t v = u64(g_base + off::HeapMgrHint);
        char txt[9]{};
        uint8_t b[8];
        read(g_base + off::HeapMgrHint, b, 8);
        for (int i = 0; i < 8; i++) txt[i] = (b[i] >= 32 && b[i] <= 126) ? (char)b[i] : '.';
        txt[8] = 0;
        printf("    0x%X -> 0x%llX   mapped=%s  ascii=\"%s\"\n", off::HeapMgrHint,
               (unsigned long long)v, mapped(v) ? "yes" : "no", txt);
        printf("    (this offset is stale on the current build, same as the rest)\n");
    }

    // ---------------------------------------------------------------------
    //  Step 2 -- find the manager by structure.
    //
    //  What makes a bucket manager recognisable: a header followed by several
    //  pointer fields that are either null or point into mapped memory, and at
    //  least one pair of them that reads as a vector -- an ordered pair p <= q
    //  with q - p a sane multiple of 8.  Scan .data for objects matching that.
    // ---------------------------------------------------------------------
    printf("\n=== looking for a bucket manager in .data ===\n");
    std::vector<uint64_t> managers;
    {
        constexpr size_t CH = 4u << 20;
        std::vector<uint8_t> buf(CH);
        for (uint64_t a = dataLo; a < dataHi; a += CH) {
            size_t want = (size_t)((dataHi - a) < CH ? (dataHi - a) : CH);
            if (!read(a, buf.data(), want)) continue;
            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t v;
                memcpy(&v, buf.data() + i, 8);
                if (!mapped(v)) continue;
                // does it carry a vector pair somewhere in its first 0x400?
                for (uint64_t o = 0; o < 0x400; o += 8) {
                    uint64_t p = u64(v + o), q = u64(v + o + 8);
                    if (!p || !q || q < p) continue;
                    if (!mapped(p) || !mapped(q)) continue;
                    uint64_t d = q - p;
                    if (d < 32 || d > (4ULL << 20) || (d % 8)) continue;
                    managers.push_back(v);
                    break;
                }
                if (managers.size() >= 400) break;
            }
            if (managers.size() >= 400) break;
        }
    }
    printf("    candidate managers: %zu\n", managers.size());

    // ---------------------------------------------------------------------
    //  Step 3 -- for each manager, look for an entity array at every vector
    //  pair it exposes, and judge the contents.
    // ---------------------------------------------------------------------
    printf("\n=== probing every vector pair in every manager ===\n");
    ArrayVerdict winner{};
    int tested = 0;
    for (uint64_t mgr : managers) {
        for (uint64_t o = 0; o < 0x600; o += 8) {
            uint64_t p = u64(mgr + o), q = u64(mgr + o + 8);
            if (!p || !q || q <= p) continue;
            if (!mapped(p) || !mapped(q)) continue;
            uint64_t bytes = q - p;
            if (bytes < 32 || bytes > (16ULL << 20) || (bytes % 8)) continue;
            tested++;
            ArrayVerdict v = probe_array(p, q);
            if (v.good && (!winner.good || v.players > winner.players)) {
                winner = v;
                winner.va = p;
            }
        }
    }
    printf("    vector pairs tested: %d\n", tested);

    if (winner.good) {
        printf("\n    ENTITY ARRAY at 0x%llX\n", (unsigned long long)winner.va);
        printf("      slots %d   resolvable %d   players %d   distinct %d   spread %.1f\n",
               winner.slots, winner.resolvable, winner.players, winner.distinct, winner.spread);
        for (int i = 0; i < 24 && i < winner.slots; i++) {
            uint64_t q = u64(winner.va + uint64_t(i) * 8);
            if (!q) { printf("        [%2d]  -\n", i); continue; }
            uint64_t ie = resolve_ient(q);
            Entity e{};
            if (!ie || !load_entity(ie, &e)) { printf("        [%2d]  unresolved\n", i); continue; }
            float d = std::sqrt((e.pos[0]-g_cam[0])*(e.pos[0]-g_cam[0]) +
                                (e.pos[1]-g_cam[1])*(e.pos[1]-g_cam[1]) +
                                (e.pos[2]-g_cam[2])*(e.pos[2]-g_cam[2]));
            printf("        [%2d]  ient 0x%llX mask %u  pos %9.2f %9.2f %9.2f  d=%7.1f%s\n",
                   i, (unsigned long long)e.ient, e.mask, e.pos[0], e.pos[1], e.pos[2], d,
                   (e.ient == g_localIent) ? "  <- you" : "");
        }
        FILE* f = fopen("entity_array.json", "w");
        if (f) {
            fprintf(f, "{\n  \"array_va\": \"0x%llX\",\n  \"slots\": %d,\n"
                       "  \"players\": %d,\n  \"entries\": [",
                    (unsigned long long)winner.va, winner.slots, winner.players);
            bool first = true;
            for (int i = 0; i < winner.slots; i++) {
                uint64_t q = u64(winner.va + uint64_t(i) * 8);
                if (!q) continue;
                uint64_t ie = resolve_ient(q);
                Entity e{};
                if (!ie || !load_entity(ie, &e) || e.mask != 2) continue;
                fprintf(f, "%s\n    { \"ient\": \"0x%llX\", \"mask\": %u, "
                           "\"pos\": [%.2f, %.2f, %.2f] }",
                        first ? "" : ",", (unsigned long long)e.ient, e.mask,
                        e.pos[0], e.pos[1], e.pos[2]);
                first = false;
            }
            fprintf(f, "\n  ]\n}\n");
            fclose(f);
            printf("\n[+] wrote entity_array.json\n");
        }
    } else {
        printf("\n    no manager exposed an entity array.\n");
        printf("\n=== result ===\n");
        printf("  The bucket manager route did not produce a player list.\n");
        printf("  Combined with the earlier array sweep, that means the player\n");
        printf("  container is not in writable memory in a shape reachable by\n");
        printf("  external reads alone.\n");
    }
    return 0;
}
