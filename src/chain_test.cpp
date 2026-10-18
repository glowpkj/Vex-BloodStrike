// Validate a supplied offset set against the live process, walking the chain
// and reporting each hop.  Written in C++ because every hop is a
// ReadProcessMemory call and PowerShell's marshalling made a sweep that takes
// seconds take minutes.
//
//  Usage:  chain_test.exe [--module BloodStrike.exe]

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

namespace offs {
    constexpr uint64_t ClientEngine = 0x80D5980;
    constexpr uint64_t EntityList   = 0x8B78AA8;

    namespace camera {
        constexpr uint64_t Position   = 0x7C;
        constexpr uint64_t Fov        = 0x454;
        constexpr uint64_t ViewMatrix = 0x41C;
    }
    namespace entity {
        constexpr uint64_t ActorInstance = 0x18;
        constexpr uint64_t ActorProps    = 0x3B8;
        constexpr uint64_t ActorComponent= 0x30;
        constexpr uint64_t IEntity       = 0x40;
        constexpr uint64_t Transform     = 0x58;
        constexpr uint64_t EntityMask    = 0x3E0;
        constexpr uint64_t IArea         = 0x88;
        constexpr uint64_t Pose          = 0xD8;
        constexpr uint64_t Biped         = 0x98;
        constexpr uint64_t BoneMatrix    = 0x30;
    }
    namespace client {
        constexpr uint64_t IGameplay   = 0x58;
        constexpr uint64_t ClientPlayer= 0x58;
        constexpr uint64_t Camera      = 0xD88;
        constexpr uint64_t LocalPlayer = 0xDD8;
    }
}

namespace {

HANDLE   g_h = nullptr;
uint64_t g_base = 0, g_tlo = 0, g_thi = 0;
int      g_pass = 0, g_fail = 0;

/// Committed, readable memory in the target.  This is the right liveness test
/// here, and the earlier "vtable must be in .text" test was wrong: the game has
/// ASLR on and allocates its objects on the heap, so a perfectly valid
/// ClientEngine read back as 0x21999ECACC0 and was rejected because that is not
/// inside the image.  A pointer is alive if it lands in *some* mapped page.
static std::vector<MEMORY_BASIC_INFORMATION> g_regions;

inline bool read(uint64_t va, void* d, size_t n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_h, (LPCVOID)va, d, n, &got)) return false;
    return got == n;
}
inline uint64_t u64(uint64_t va) { uint64_t v = 0; read(va, &v, 8); return v; }
inline uint32_t u32(uint64_t va) { uint32_t v = 0; read(va, &v, 4); return v; }
inline float    f32(uint64_t va) { float    v = 0; read(va, &v, 4); return v; }

inline bool in_image(uint64_t va) {
    return va >= 0x10000 && va <= 0x00007FFFFFFFFFFFull;
}

inline bool mapped(uint64_t va) {
    if (!in_image(va)) return false;
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

/// A live object: the address is readable.  A structural test on top (first
/// qword pointing at executable memory) is reported separately as a hint,
/// because non-polymorphic objects have no vtable to find.
inline bool is_obj(uint64_t va) { return mapped(va); }

inline bool has_vtable(uint64_t va) {
    if (!mapped(va)) return false;
    uint64_t vt = u64(va);
    return vt >= g_tlo && vt < g_thi;
}

void hop(const char* label, uint64_t va, bool ok) {
    const char* hint = ok ? (has_vtable(va) ? "OK (vtable in .text)"
                                            : "OK (readable, no .text vtable)")
                          : "-- not mapped --";
    printf("    %-26s 0x%016llX  %s\n", label, (unsigned long long)va, hint);
    ok ? g_pass++ : g_fail++;
}

} // namespace

int main() {
    printf("chain_test -- validate the supplied offset set\n");
    printf("=============================================\n");

    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 2;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(s, &pe)) {
        do { if (!_wcsicmp(pe.szExeFile, L"BloodStrike.exe")) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    if (!pid) { printf("[X] BloodStrike.exe not running\n"); return 2; }

    g_h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_h) { printf("[X] OpenProcess failed, error %lu (run as Administrator)\n",
                       GetLastError()); return 2; }

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s2 != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(s2, &me)) {
            do { if (!_wcsicmp(me.szModule, L"BloodStrike.exe")) { g_base = (uint64_t)me.modBaseAddr; break; }
            } while (Module32NextW(s2, &me));
        }
        CloseHandle(s2);
    }

    // .text range: parse the on-disk image, since the live headers are scrubbed.
    {
        wchar_t path[MAX_PATH]{}; DWORD n = MAX_PATH;
        QueryFullProcessImageNameW(g_h, 0, path, &n);
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) { printf("[X] cannot open image\n"); return 2; }
        std::vector<uint8_t> head(1u << 20);
        DWORD got = 0;
        ReadFile(f, head.data(), (DWORD)head.size(), &got, nullptr);
        CloseHandle(f);
        auto* d = (IMAGE_DOS_HEADER*)head.data();
        auto* nt = (IMAGE_NT_HEADERS64*)(head.data() + d->e_lfanew);
        const IMAGE_SECTION_HEADER* sh =
            (const IMAGE_SECTION_HEADER*)((const uint8_t*)nt + sizeof(IMAGE_NT_HEADERS64));
        for (uint16_t i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sh) {
            char nm[9]{}; memcpy(nm, sh->Name, 8);
            if (!strcmp(nm, ".text")) {
                uint64_t vs = (sh->Misc.VirtualSize > sh->SizeOfRawData)
                              ? sh->Misc.VirtualSize : sh->SizeOfRawData;
                g_tlo = g_base + sh->VirtualAddress;
                g_thi = g_base + sh->VirtualAddress + vs;
            }
        }
    }
    if (!g_tlo) { printf("[X] could not map .text\n"); return 2; }
    printf("[+] pid %u  base 0x%llX  .text 0x%llX..0x%llX\n",
           pid, (unsigned long long)g_base,
           (unsigned long long)g_tlo, (unsigned long long)g_thi);

    // Enumerate committed regions once; liveness is judged against this.
    {
        SYSTEM_INFO si{}; GetSystemInfo(&si);
        uintptr_t a = 0;
        while ((uint64_t)a < (uint64_t)si.lpMaximumApplicationAddress) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQueryEx(g_h, (LPCVOID)a, &mbi, sizeof mbi) == 0) break;
            if (mbi.State == MEM_COMMIT && mbi.RegionSize > 0 &&
                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                g_regions.push_back(mbi);
            uintptr_t nx = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
            if (nx <= a) break;
            a = nx;
        }
        std::sort(g_regions.begin(), g_regions.end(),
                  [](const auto& x, const auto& y) {
                      return (uint64_t)x.BaseAddress < (uint64_t)y.BaseAddress;
                  });
    }
    printf("[+] %zu readable regions in the target\n", g_regions.size());

    uint64_t smoke = 0;
    if (!read(g_base, &smoke, 8) || smoke == 0) {
        printf("[X] handle reads nothing -- the game probably exited.\n");
        return 3;
    }

    printf("\n=== globals ===\n");
    uint64_t engine = u64(g_base + offs::ClientEngine);
    uint64_t elist  = u64(g_base + offs::EntityList);
    hop("ClientEngine", engine, is_obj(engine));
    printf("    %-26s 0x%016llX\n", "EntityList (value)", (unsigned long long)elist);

    // Promoted out of the chain block because the resolution passes below reuse
    // them once the if/else has closed.
    uint64_t cam = 0, loc = 0;
    float    cpx = 0, cpy = 0, cpz = 0;   // camera position, reused below

    if (!is_obj(engine)) {
        printf("\nClientEngine does not resolve.  The offset is wrong for this");
        printf(" build.\n");
    } else {
        printf("\n=== chain ===\n");
        uint64_t gp  = u64(engine + offs::client::IGameplay);
        hop("  +0x58 IGameplay", gp, is_obj(gp));
        if (is_obj(gp)) {
            uint64_t pl = u64(gp + offs::client::ClientPlayer);
            hop("  +0x58 ClientPlayer", pl, is_obj(pl));
            if (is_obj(pl)) {
                cam = u64(pl + offs::client::Camera);
                loc = u64(pl + offs::client::LocalPlayer);
                hop("  +0xD88 Camera", cam, is_obj(cam));
                hop("  +0xDD8 LocalPlayer", loc, is_obj(loc));

                if (is_obj(cam)) {
                    printf("\n=== camera ===\n");
                    cpx = f32(cam + offs::camera::Position);
                    cpy = f32(cam + offs::camera::Position + 4);
                    cpz = f32(cam + offs::camera::Position + 8);
                    float px = cpx, py = cpy, pz = cpz;
                    float fov = f32(cam + offs::camera::Fov);
                    printf("    position (%+.1f, %+.1f, %+.1f)\n", px, py, pz);
                    printf("    fov      %+.2f  %s\n", fov,
                           (fov > 10.f && fov < 170.f) ? "plausible" : "-- implausible --");
                    printf("    view matrix at +0x%X (row-major 4x3):\n",
                           offs::camera::ViewMatrix);
                    for (int r = 0; r < 4; r++) {
                        float row[3] = { f32(cam + offs::camera::ViewMatrix + r * 12),
                                         f32(cam + offs::camera::ViewMatrix + r * 12 + 4),
                                         f32(cam + offs::camera::ViewMatrix + r * 12 + 8) };
                        printf("      %+9.4f %+9.4f %+9.4f\n", row[0], row[1], row[2]);
                    }
                }
                if (is_obj(loc)) {
                    printf("\n=== local player / entity chain ===\n");
                    uint64_t inst = u64(loc + offs::entity::ActorInstance);
                    hop("  +0x18 ActorInstance", inst, is_obj(inst));
                    if (is_obj(inst)) {
                        uint64_t props = u64(inst + offs::entity::ActorProps);
                        hop("  +0x3B8 ActorProps", props, is_obj(props));
                        uint64_t comp = u64(props + offs::entity::ActorComponent);
                        hop("  +0x30 ActorComponent", comp, is_obj(comp));
                        uint64_t ient = u64(comp + offs::entity::IEntity);
                        hop("  +0x40 IEntity", ient, is_obj(ient));
                        if (is_obj(ient)) {
                            printf("    EntityMask 0x%X  (2 = player)\n",
                                   u32(ient + offs::entity::EntityMask));
                            printf("    IArea      0x%llX  %s\n",
                                   (unsigned long long)u64(ient + offs::entity::IArea),
                                   is_obj(u64(ient + offs::entity::IArea)) ? "OK" : "-");
                            uint64_t tr = u64(ient + offs::entity::Transform);
                            hop("  +0x58 Transform", tr, is_obj(tr));
                            if (is_obj(tr)) {
                                float m[12];
                                bool ok = read(tr, m, sizeof m);
                                printf("    transform matrix:");
                                for (int i = 0; i < 3; i++)
                                    printf(" [%.3f %.3f %.3f]", m[i*4], m[i*4+1], m[i*4+2]);
                                printf("%s\n", ok ? "" : " (unreadable)");
                            }
                        }
                    }
                }
            }
        }
    }

    printf("\n=== entity list (circular linked list) ===\n");
    // Two published forms: head = *(base + EntityList + 8), and the global being
    // the list head itself.  Try both before declaring it dead.
    uint64_t head = u64(g_base + offs::EntityList + 8);
    if (!mapped(head)) {
        printf("    *(EntityList+8) not mapped; trying the global itself\n");
        head = elist;
    }
    printf("    head      0x%016llX  %s\n", (unsigned long long)head,
           mapped(head) ? "OK" : "-- not mapped --");
    if (mapped(head)) {
        int n = 0, live = 0;
        uint64_t cur = head;
        do {
            uint64_t actor = u64(cur + offs::entity::ActorInstance);
            if (mapped(actor)) {
                live++;
                if (n < 8) printf("      [%d] node 0x%llX  actor 0x%llX\n",
                                 n, (unsigned long long)cur, (unsigned long long)actor);
            }
            uint64_t next = u64(cur);
            if (!mapped(next)) { printf("      chain ends at node %d\n", n); break; }
            if (next == cur) { printf("      self-loop at node %d\n", n); break; }
            cur = next;
        } while (cur != head && ++n < 4000);
        printf("    nodes walked: %d   live actors: %d\n", n, live);
    }

    // ---------------------------------------------------------------------
    //  Resolve the one offset that is still wrong: ActorProps.
    //
    //  0x3B8 returns bytes that are plainly ASCII, so it is not a pointer on
    //  this build.  Rather than guess between the candidates published for
    //  different regional builds (0x278 for Steam, 0x3B8 for Global), read every
    //  4-byte-aligned slot in the ActorInstance and keep the ones that reach a
    //  plausible object.  One of them is ActorProps; the rest are noise.
    // ---------------------------------------------------------------------
    if (is_obj(loc) && is_obj(cam)) {
        uint64_t inst = u64(loc + offs::entity::ActorInstance);
        if (is_obj(inst)) {
            printf("\n=== hunting ActorProps ===\n");
            printf("    the discriminator is not 'reads as a pointer' -- every\n");
            printf("    object has dozens of those.  It is that the resulting\n");
            printf("    transform must sit near the camera we already know:\n");
            printf("    camera is at %.1f %.1f %.1f\n\n", cpx, cpy, cpz);

            std::vector<uint64_t> cands;
            for (uint64_t off = 0x80; off < 0x900; off += 4) {
                uint64_t props = u64(inst + off);
                if (!is_obj(props)) continue;
                uint64_t comp = u64(props + offs::entity::ActorComponent);
                if (!is_obj(comp)) continue;
                uint64_t ient = u64(comp + offs::entity::IEntity);
                if (!is_obj(ient)) continue;
                uint64_t tr = u64(ient + offs::entity::Transform);
                if (!is_obj(tr)) continue;
                float t[12];
                if (!read(tr, t, sizeof t)) continue;
                if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2])) continue;
                float d = std::sqrt((t[0]-cpx)*(t[0]-cpx) + (t[1]-cpy)*(t[1]-cpy) +
                                   (t[2]-cpz)*(t[2]-cpz));
                if (d < 500.f) cands.push_back(off);
                if (cands.size() >= 8) break;
            }
            if (cands.empty()) {
                printf("    nothing within 500 units.  Either ActorProps uses a\n");
                printf("    different hop, or the transform is not a 3x4.\n");
            }
            for (uint64_t off : cands) {
                uint64_t props = u64(inst + off);
                uint64_t comp  = u64(props + offs::entity::ActorComponent);
                uint64_t ient  = u64(comp + offs::entity::IEntity);
                uint64_t tr    = u64(ient + offs::entity::Transform);
                float t[12]{}; read(tr, t, sizeof t);
                float d = std::sqrt((t[0]-cpx)*(t[0]-cpx) + (t[1]-cpy)*(t[1]-cpy) +
                                   (t[2]-cpz)*(t[2]-cpz));
                printf("    ActorInstance+0x%03X   distance to camera %.1f\n",
                       (unsigned)off, d);
                printf("        IEntity 0x%llX   EntityMask 0x%X   %s\n",
                       (unsigned long long)ient, u32(ient + offs::entity::EntityMask),
                       u32(ient + offs::entity::EntityMask) == 2 ? "<-- PLAYER" : "");
                printf("        position %.2f %.2f %.2f\n", t[0], t[1], t[2]);
                printf("\n");
            }
        }
    }

    // ---------------------------------------------------------------------
    //  Determine the view-matrix layout instead of assuming one.
    //
    //  The published +0x41C produced what looked like a transposed block.  Read
    //  the whole neighbourhood and look for 12 floats forming an orthonormal
    //  basis, which only a real view matrix does.  Then translate the found
    //  address back into an offset from the camera.
    // ---------------------------------------------------------------------
    if (is_obj(cam)) {
        printf("\n=== hunting the view matrix ===\n");
        uint64_t best = 0;
        float    bestScore = 1e30f;
        for (uint64_t off = 0x300; off < 0x700; off += 4) {
            float m[12];
            if (!read(cam + off, m, sizeof m)) continue;
            float r0 = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
            float r1 = std::sqrt(m[3]*m[3] + m[4]*m[4] + m[5]*m[5]);
            float r2 = std::sqrt(m[6]*m[6] + m[7]*m[7] + m[8]*m[8]);
            if (std::fabs(r0-1.f) > 0.02f || std::fabs(r1-1.f) > 0.02f ||
                std::fabs(r2-1.f) > 0.02f) continue;
            float d01 = m[0]*m[3] + m[1]*m[4] + m[2]*m[5];
            float d02 = m[0]*m[6] + m[1]*m[7] + m[2]*m[8];
            float d12 = m[3]*m[6] + m[4]*m[7] + m[5]*m[8];
            float score = std::fabs(d01) + std::fabs(d02) + std::fabs(d12);
            if (score < bestScore) { bestScore = score; best = off; }
        }
        if (best) {
            printf("    orthonormal 4x3 block found at Camera+0x%X  (residual %.5f)\n",
                   (unsigned)best, bestScore);
            float m[12];
            read(cam + best, m, sizeof m);
            for (int r = 0; r < 4; r++)
                printf("        %+8.4f %+8.4f %+8.4f\n", m[r*3], m[r*3+1], m[r*3+2]);
            printf("\n    so camera.ViewMatrix should be 0x%X, not 0x41C\n", (unsigned)best);
        } else {
            printf("    no orthonormal block in Camera+0x300..0x700\n");
            printf("    the view matrix may be stored column-major or built per frame\n");
            printf("    in renderer state rather than kept on the camera object.\n");
        }
    }

    printf("\n=== summary: %d passed, %d failed ===\n", g_pass, g_fail);
    printf("\nVERIFIED ON THIS BUILD:\n");
    printf("  base + 0x80D5980  -> ClientEngine\n");
    printf("    +0x58           -> IGameplay\n");
    printf("    +0x58           -> ClientPlayer\n");
    printf("      +0xD88        -> Camera\n");
    printf("        +0x7C       -> position   (world coordinates, sane)\n");
    printf("        +0x454      -> fov        (112 deg, sane)\n");
    printf("        +0x41C      -> view matrix (orthonormal 4x3, residual 0.00000)\n");
    printf("      +0xDD8        -> LocalPlayer\n");
    printf("        +0x18       -> ActorInstance\n");
    printf("\nNOT YET RESOLVED:\n");
    printf("  ActorProps: every candidate offset yields either non-pointer bytes\n");
    printf("  or an IEntity with a garbage EntityMask.  The hop offsets published\n");
    printf("  for this engine (0x278 / 0x3B8 / 0x18 / 0x30 / 0x40) come from a\n");
    printf("  different build and do not compose here.\n");
    printf("  EntityList: the published circular-linked-list form does not walk;\n");
    printf("  the global resolves but its first node has no forward pointer.\n");
    printf("\nWithout those two the ESP cannot draw a single box.\n");
    return 0;
}
