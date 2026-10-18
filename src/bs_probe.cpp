// -----------------------------------------------------------------------------
//  bs_probe.cpp  --  runtime discovery for Blood Strike (PC).
//
//  Static analysis is exhausted for this target.  The protector strips the
//  vtable[-1] -> CompleteObjectLocator backlink and encrypts the vftables, so
//  the ~21k RTTI class names in the file cannot be connected to any code on
//  disk, and only 82 survive in memory.  Everything useful exists only while the
//  game is running.
//
//  What this tool does, in order:
//
//    0  dump the loaded modules that are NOT on disk.  tier0_s64.dll and
//       vstdlib_s64.dll are unpacked from the .mpk containers at runtime and
//       never touch the filesystem, so the only way to analyse them is to lift
//       them out of memory.
//    1  find the view-projection matrix.  Rows 0..2 of VP are mutually
//       orthogonal and their norms are the focal lengths, which is a
//       convention-independent signature -- unlike guessing whether the engine
//       stores the w-marker at m[10] or m[14].
//    2  find the entity container.
//    3  find the position field inside those entities.
//    4  find health / team next to it.
//
//  Output: bs_probe.json, plus any dumped DLLs.
//
//  Build:  cmake --build build --config Release --target bs_probe
//  Run:    game running first, then:  bs_probe.exe
// -----------------------------------------------------------------------------

#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <array>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace {

std::wstring widen(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

struct Section {
    char     name[16]{};
    uint32_t rva = 0, vsize = 0, chars = 0;
    bool readable() const { return (chars & 0x40000000) != 0; }
    bool exec()     const { return (chars & 0x20000000) != 0; }
};

inline bool finite3(const float* v) {
    for (int i = 0; i < 3; i++) if (!std::isfinite(v[i])) return false;
    return true;
}
inline float vlen(const float* v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}
inline float vdot(const float* a, const float* b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

} // namespace

namespace bs {

// ================================================================== process
class Proc {
public:
    ~Proc() { detach(); }

    bool attach(const wchar_t* module_name);
    void detach() { if (h_) { CloseHandle(h_); h_ = nullptr; } }

    bool read(uint64_t va, void* dst, size_t len) const {
        SIZE_T got = 0;
        if (!ReadProcessMemory(h_, (LPCVOID)va, dst, len, &got)) return false;
        return got == len;
    }
    template <typename T> bool read_va(uint64_t va, T& out) const {
        return read(va, &out, sizeof(T));
    }
    template <typename T> T get(uint64_t va, T def = T{}) const {
        T v = def; read(va, &v, sizeof(T)); return v;
    }

    bool      ok() const { return h_ != nullptr; }
    DWORD     pid() const { return pid_; }
    uint64_t  base() const { return base_; }
    uint64_t  rva_of(uint64_t va) const { return va - base_; }

    const Section* section_by_name(const char* n) const;
    const Section* section_of_rva(uint32_t rva) const;
    std::vector<Section> sections;

    struct Region { uint64_t lo = 0, hi = 0; };
    std::vector<Region> rw_regions() const;
    std::vector<Region> all_regions() const;

    bool load_sections_from_disk();

    std::vector<std::pair<std::string, uint64_t>> modules() const;   // name, size

private:
    HANDLE  h_ = nullptr;
    DWORD   pid_ = 0;
    uint64_t base_ = 0;
};

bool Proc::attach(const wchar_t* module_name) {
    DWORD found = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, module_name)) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (!found) return false;

    pid_ = found;
    h_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid_);
    if (!h_) return false;

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
    if (s2 != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(s2, &me)) {
            do {
                if (!_wcsicmp(me.szModule, module_name)) {
                    base_ = (uint64_t)me.modBaseAddr;
                    break;
                }
            } while (Module32NextW(s2, &me));
        }
        CloseHandle(s2);
    }
    if (!base_) return false;
    return load_sections_from_disk();
}

bool Proc::load_sections_from_disk() {
    wchar_t path[MAX_PATH]{};
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(h_, 0, path, &n)) return false;

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    std::vector<uint8_t> head(0x20000);
    DWORD got = 0;
    bool ok = false;
    if (ReadFile(f, head.data(), (DWORD)head.size(), &got, nullptr) && got > 0x200) {
        auto* dos = (IMAGE_DOS_HEADER*)head.data();
        if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
            dos->e_lfanew > 0 && dos->e_lfanew + 0x200 < (LONG)got) {
            auto* nt = (IMAGE_NT_HEADERS64*)(head.data() + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                const IMAGE_SECTION_HEADER* sh =
                    (const IMAGE_SECTION_HEADER*)((const uint8_t*)nt + sizeof(IMAGE_NT_HEADERS64));
                uint16_t cnt = nt->FileHeader.NumberOfSections;
                if (cnt > 0 && cnt < 96 && (const uint8_t*)(sh + cnt) <= head.data() + got) {
                    sections.resize(cnt);
                    for (uint16_t i = 0; i < cnt; ++i, ++sh) {
                        memcpy(sections[i].name, sh->Name, 8);
                        sections[i].name[8] = 0;
                        sections[i].rva   = sh->VirtualAddress;
                        sections[i].vsize = (sh->Misc.VirtualSize > sh->SizeOfRawData)
                                          ? sh->Misc.VirtualSize : sh->SizeOfRawData;
                        sections[i].chars = sh->Characteristics;
                    }
                    ok = true;
                }
            }
        }
    }
    CloseHandle(f);
    return ok;
}

const Section* Proc::section_by_name(const char* n) const {
    for (const auto& s : sections) if (!strcmp(s.name, n)) return &s;
    return nullptr;
}

const Section* Proc::section_of_rva(uint32_t rva) const {
    for (const auto& s : sections)
        if (rva >= s.rva && rva < s.rva + s.vsize) return &s;
    return nullptr;
}

std::vector<Proc::Region> Proc::rw_regions() const {
    std::vector<Region> out;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uintptr_t addr = 0;
    const uint64_t img_lo = base_, img_hi = base_ + 0x20000000ULL;
    while ((uint64_t)addr < (uint64_t)si.lpMaximumApplicationAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(h_, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;
        uint64_t lo = (uint64_t)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) &&
            !(lo >= img_lo && lo < img_hi) &&
            mbi.RegionSize > 0 && mbi.RegionSize < (1ULL << 34))
            out.push_back({ lo, lo + mbi.RegionSize });
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return out;
}

std::vector<Proc::Region> Proc::all_regions() const {
    std::vector<Region> out;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    uintptr_t addr = 0;
    while ((uint64_t)addr < (uint64_t)si.lpMaximumApplicationAddress) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQueryEx(h_, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_COMMIT && mbi.RegionSize > 0 && mbi.RegionSize < (1ULL << 34) &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            out.push_back({ (uint64_t)mbi.BaseAddress, (uint64_t)mbi.BaseAddress + mbi.RegionSize });
        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    return out;
}

std::vector<std::pair<std::string, uint64_t>> Proc::modules() const {
    std::vector<std::pair<std::string, uint64_t>> out;
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_);
    if (ms == INVALID_HANDLE_VALUE) return out;
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(ms, &me)) {
        do {
            char nm[MAX_PATH]{};
            WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, nm, MAX_PATH, 0, 0);
            out.push_back({ nm, (uint64_t)me.modBaseSize });
        } while (Module32NextW(ms, &me));
    }
    CloseHandle(ms);
    return out;
}

// ================================================================ discovery
class Discovery {
public:
    explicit Discovery(Proc& p) : p_(p) {}
    void run();

private:
    void stage0_modules();
    void stage1_viewproj();
    void stage2_entities();
    void stage3_position();
    void stage4_scalars();
    void write_json();
    void note(const char* fmt, ...);

    Proc& p_;
    std::vector<Proc::Region> regs_, all_;

    bool    vp_ok_ = false;
    uint64_t vp_va_ = 0;
    float   vp_[16]{};
    float   vp_fov_ = 0.f;

    struct EntArray { uint64_t va; int span; int live; };
    std::vector<EntArray> arrays_;

    struct PosCand { uint32_t off; int objects; float mx, my, mz; };
    std::vector<PosCand> pos_;

    struct ScalarHit { uint32_t off; int in_range, changed, byte_team; };
    std::vector<ScalarHit> scalars_;

    std::vector<std::string> notes_;
};

void Discovery::note(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    printf("%s\n", b);
    fflush(stdout);
}

// -----------------------------------------------------------------------------
//  Stage 0 -- lift the modules that only exist in memory.
//
//  tier0_s64.dll and vstdlib_s64.dll are Valve's Source 2 runtime, and they are
//  NOT in the install directory: the game unpacks them from the .mpk containers
//  at start-up.  Anything worth reverse engineering about the renderer or the
//  object model lives in those two files, and the only way to see them is to
//  copy them out of the running process.
// -----------------------------------------------------------------------------
void Discovery::stage0_modules() {
    note("\n=== stage 0  modules that exist only in memory ===");

    auto mods = p_.modules();
    note("    %zu modules loaded", mods.size());

    struct Dump { char name[MAX_PATH]; uint64_t base; uint64_t size; char disk[MAX_PATH]; };
    std::vector<Dump> dumps;
    HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, p_.pid());
    if (ms != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(ms, &me)) {
            do {
                char nm[MAX_PATH]{};
                WideCharToMultiByte(CP_ACP, 0, me.szModule, -1, nm, MAX_PATH, 0, 0);
                char disk[MAX_PATH]{};
                WideCharToMultiByte(CP_ACP, 0, me.szExePath, -1, disk, MAX_PATH, 0, 0);
                bool onDisk = GetFileAttributesA(disk) != INVALID_FILE_ATTRIBUTES;
                bool isGame = _stricmp(nm, "BloodStrike.exe") == 0;
                if (onDisk || isGame) continue;
                if (me.modBaseSize < 64 * 1024) continue;
                Dump d{};
                strncpy_s(d.name, nm, _TRUNCATE);
                strncpy_s(d.disk, disk, _TRUNCATE);
                d.base = (uint64_t)me.modBaseAddr;
                d.size = me.modBaseSize;
                dumps.push_back(d);
            } while (Module32NextW(ms, &me));
        }
        CloseHandle(ms);
    }

    if (dumps.empty()) {
        note("    every loaded module exists on disk -- nothing to dump.");
        return;
    }

    for (const auto& d : dumps) {
        char out[MAX_PATH]{};
        sprintf_s(out, "dumped_%s", d.name);
        FILE* f = fopen(out, "wb");
        if (!f) { note("    %-28s cannot write", d.name); continue; }

        std::vector<uint8_t> chunk(1u << 20);
        uint64_t done = 0;
        while (done < d.size) {
            size_t want = (size_t)std::min<uint64_t>(chunk.size(), d.size - done);
            if (!p_.read(d.base + done, chunk.data(), want)) break;
            fwrite(chunk.data(), 1, want, f);
            done += want;
        }
        fclose(f);
        note("    %-28s %8.2f MB -> %s", d.name, done / 1048576.0, out);
    }
    note("    these were unpacked from the .mpk containers; analyse them");
    note("    statically now that they exist on disk.");
}

// -----------------------------------------------------------------------------
//  Stage 1 -- the view-projection matrix.
//
//  Two earlier attempts failed, both because the signature was guessed rather
//  than derived.  A projection's w-marker sits at m[14] in D3D and m[10] in
//  OpenGL, moves again under reversed-Z, and moves again when the matrix is
//  stored column-major -- so testing one fixed element is a coin flip.  The first
//  attempt scored the identity matrix, because it only asked that *some* element
//  equal +-1.
//
//  The invariant that does not depend on convention: for VP = P * V, rows 0..2
//  are f/a*r0, f*r1 and A*r2 + B*e3, so their 3D parts are mutually orthogonal
//  and the norms of rows 0 and 1 are the focal lengths.  That holds for every
//  convention, every far-plane choice, both storage orders.  Orthogonality is
//  what kills the quaternion noise the earlier stages kept selecting.
// -----------------------------------------------------------------------------
void Discovery::stage1_viewproj() {
    note("\n=== stage 1  view-projection matrix by row orthogonality ===");

    const Section* t = p_.section_by_name(".text");
    if (!t) { note("[!] no .text"); return; }
    const uint64_t tlo = t->rva, thi = uint64_t(t->rva) + t->vsize;

    // A hit: address, the 16 floats, the two focal norms, aspect estimate.
    struct Hit { uint64_t va; float m[16]; float fy; float aspect; };
    std::vector<Hit> hits;

    std::vector<uint8_t> buf(4u << 20);
    const uint64_t deadline = GetTickCount64() + 90'000;

    for (const auto& rg : regs_) {
        if (GetTickCount64() > deadline) break;
        for (uint64_t a = rg.lo; a < rg.hi; a += buf.size()) {
            if (GetTickCount64() > deadline) break;
            size_t want = (size_t)std::min<uint64_t>(buf.size(), rg.hi - a);
            if (want < 64 || !p_.read(a, buf.data(), want)) continue;

            for (size_t i = 0; i + 64 <= want; i += 4) {
                const float* m = (const float*)(buf.data() + i);
                bool finite = true;
                for (int k = 0; k < 16; k++) if (!std::isfinite(m[k])) { finite = false; break; }
                if (!finite) continue;

                // rows, 3D parts only
                const float* r0 = m;
                const float* r1 = m + 4;
                const float* r2 = m + 8;
                if (!finite3(r0) || !finite3(r1) || !finite3(r2)) continue;

                // the two focal rows must be unit-scaled, not tiny or huge
                float n0 = vlen(r0), n1 = vlen(r1);
                if (n0 < 0.25f || n0 > 6.f) continue;
                if (n1 < 0.25f || n1 > 6.f) continue;
                // and near the same scale, or the aspect is nonsense
                float aspect = n1 / n0;
                if (aspect < 0.4f || aspect > 2.6f) continue;

                // mutual orthogonality of the three 3D rows -- this is the
                // discriminator that excludes quaternions and unit-axis slots
                if (std::fabs(vdot(r0, r1)) > 2e-3f) continue;
                if (std::fabs(vdot(r0, r2)) > 2e-3f) continue;
                if (std::fabs(vdot(r1, r2)) > 2e-3f) continue;

                // the third row is A*r2 + B*e3, so its 3D norm is |A|; require
                // it to be finite and non-degenerate
                float n2 = vlen(r2);
                if (n2 < 1e-3f || n2 > 1e4f) continue;

                // the homogeneous row: either a camera depth (VP) or all-zero
                // beyond the marker (a bare projection).  Reject pure garbage:
                // m[3] and m[7] are the -dot(row, eye) terms, so if the matrix
                // has no translation at all it is a bare P, which is fine, but
                // a matrix whose w-row is arbitrary float noise is not.
                float wh[4] = { m[3], m[7], m[11], m[15] };
                int zeros = 0;
                for (int k = 0; k < 4; k++) if (wh[k] == 0.f) zeros++;
                bool bareP = (zeros >= 3);
                if (!bareP) {
                    // camera-space: the w row must be dominated by exactly one
                    // term, which is what -dot(r2, eye) looks like
                    float mx = 0.f;
                    for (int k = 0; k < 4; k++) mx = std::max(mx, std::fabs(wh[k]));
                    if (mx < 1e-3f || mx > 1e5f) continue;
                }

                Hit h{};
                h.va = a + i;
                memcpy(h.m, m, sizeof(h.m));
                h.fy = n1;
                h.aspect = aspect;
                hits.push_back(h);
            }
        }
    }
    note("    orthogonal-row candidates        : %zu", hits.size());
    if (hits.empty()) {
        note("    none.  That is a real result, not a bug: it means no 4x4 block");
        note("    in writable memory has three mutually orthogonal focal rows,");
        note("    so the renderer is not keeping the matrix as a plain float[16]");
        note("    there -- it is likely packed in SIMD registers and flushed once");
        note("    per frame, or behind an offset from its buffer base.");
        return;
    }

    // A bare projection is constant while a VP tracks the camera.  Sample both.
    note("    >>> MOVE THE MOUSE AND LOOK AROUND FOR THE NEXT 2 SECONDS <<<");
    fflush(stdout);
    if (hits.size() > 4096) hits.resize(4096);
    std::vector<std::array<float, 16>> snap(hits.size());
    for (size_t i = 0; i < hits.size(); i++)
        if (!p_.read(hits[i].va, snap[i].data(), 64)) snap[i].fill(0.f);
    Sleep(2000);

    int    bestVp   = -1;
    float  bestMove = 1e-4f;
    int    nStable  = 0;
    for (size_t i = 0; i < hits.size(); i++) {
        float now[16];
        if (!p_.read(hits[i].va, now, sizeof(now))) continue;
        float d = 0;
        for (int k = 0; k < 16; k++) d = std::max(d, std::fabs(now[k] - snap[i][k]));
        if (d < 1e-6f) { nStable++; continue; }
        if (d > bestMove) { bestMove = d; bestVp = (int)i; }
    }
    note("    unchanged over 2 s (bare projections) : %d", nStable);
    note("    largest movement                      : %.4f", bestMove);

    if (bestVp < 0) {
        note("\n    nothing moved.  The renderer rebuilds the matrix every frame at");
        note("    a fresh address, so it cannot be found by tracking a location.");
        note("    The %d stable candidates are bare projection matrices; printing", nStable);
        note("    the best few, since the focal length alone identifies the FOV.");
    }

    // Report the stable ones regardless: their focal length gives the FOV.
    std::vector<size_t> stableIdx;
    for (size_t i = 0; i < hits.size(); i++) {
        float now[16];
        if (!p_.read(hits[i].va, now, sizeof(now))) continue;
        float d = 0;
        for (int k = 0; k < 16; k++) d = std::max(d, std::fabs(now[k] - snap[i][k]));
        if (d < 1e-6f) stableIdx.push_back(i);
    }
    if (!stableIdx.empty()) {
        note("\n    bare projections (focal length -> fov):");
        for (size_t j = 0; j < stableIdx.size() && j < 8; j++) {
            const Hit& h = hits[stableIdx[j]];
            float fov = 2.f * std::atan(1.f / h.fy) * 57.2957795f;
            note("      0x%llX  fy %.5f  fx %.5f  aspect %.3f  fov %.1f deg",
                 (unsigned long long)h.va, h.fy, h.fy / h.aspect, h.aspect, fov);
        }
    }

    if (bestVp >= 0) {
        const Hit& h = hits[bestVp];
        vp_ok_ = true;
        vp_va_ = h.va;
        memcpy(vp_, h.m, sizeof(vp_));
        vp_fov_ = 2.f * std::atan(1.f / h.fy) * 57.2957795f;
        note("\n    VIEW-PROJECTION at 0x%llX", (unsigned long long)vp_va_);
        note("    fov %.1f deg   aspect %.3f", vp_fov_, h.aspect);
        note("    matrix (row-major, as stored)");
        for (int r = 0; r < 4; r++)
            note("      %11.5f %11.5f %11.5f %11.5f",
                 h.m[r * 4], h.m[r * 4 + 1], h.m[r * 4 + 2], h.m[r * 4 + 3]);
        note("    eye from the w row: %.1f, %.1f, %.1f",
             -h.m[3] / std::max(1e-6f, h.fy / h.aspect),
             -h.m[7] / std::max(1e-6f, h.fy),
             -h.m[11]);
    }
}

// -----------------------------------------------------------------------------
//  Stage 2 -- the entity container.
//
//  Deliberately reports a histogram of what it saw even when nothing matches.
//  Tuning a threshold without seeing the distribution is guessing, and two
//  earlier thresholds were guessed.
// -----------------------------------------------------------------------------
void Discovery::stage2_entities() {
    note("\n=== stage 2  entity container ===");

    const Section* t = p_.section_by_name(".text");
    if (!t) return;
    const uint64_t tlo = t->rva, thi = uint64_t(t->rva) + t->vsize;

    auto mapped = [&](uint64_t a) -> bool {
        if (a < 0x10000 || a > 0x00007FFFFFFFFFFFULL) return false;
        size_t lo = 0, hi = all_.size();
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            if (a < all_[mid].lo) hi = mid;
            else if (a >= all_[mid].hi) lo = mid + 1;
            else return true;
        }
        return false;
    };
    auto live_entity = [&](uint64_t e) -> bool {
        if (!mapped(e)) return false;
        uint64_t vt = p_.get<uint64_t>(e);
        if (vt < 0x10000) return false;
        return vt - p_.base() >= tlo && vt - p_.base() < thi;
    };

    // Histogram: how many runs of N consecutive pointer-ish slots exist.
    const int kBuckets[] = { 16, 64, 256, 1024, 4096, 16384 };
    uint64_t hist[6] = { 0 };
    std::vector<EntArray> cands;

    std::vector<uint8_t> buf(4u << 20);
    for (const auto& rg : regs_) {
        for (uint64_t a = rg.lo; a < rg.hi; a += buf.size()) {
            size_t want = (size_t)std::min<uint64_t>(buf.size(), rg.hi - a);
            if (want < 128 || !p_.read(a, buf.data(), want)) continue;
            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t q;
                memcpy(&q, buf.data() + i, 8);
                if (q == 0) continue;
                if (!mapped(q)) continue;
                // measure the run length starting here
                int n = 0;
                for (size_t j = i; j + 8 <= want; j += 8) {
                    uint64_t v;
                    memcpy(&v, buf.data() + j, 8);
                    if (v == 0) { n++; continue; }
                    if (mapped(v)) n++;
                    else break;
                }
                for (int b = 0; b < 6; b++) if (n >= kBuckets[b]) hist[b]++;
                if (n >= 64) {
                    int live = 0;
                    for (int k = 0; k < n && k < 2048; k++) {
                        uint64_t v;
                        memcpy(&v, buf.data() + i + size_t(k) * 8, 8);
                        if (v && live_entity(v)) live++;
                    }
                    if (live >= 8)
                        cands.push_back({ a + i, n, live });
                }
                i += size_t(n ? n : 1) * 8 - 8;   // skip the run
            }
        }
    }

    note("    runs of pointer-like slots, by minimum length:");
    for (int b = 0; b < 6; b++)
        note("      >= %6d : %llu", kBuckets[b], (unsigned long long)hist[b]);

    std::sort(cands.begin(), cands.end(), [](const EntArray& a, const EntArray& b) {
        if (a.live != b.live) return a.live > b.live;
        return a.span > b.span;
    });
    note("    arrays with >= 8 live vtable-backed entries : %zu", cands.size());

    for (size_t i = 0; i < cands.size() && i < 6; i++) {
        note("\n    array at 0x%llX : %d slots, %d live (%.1f%%)",
             (unsigned long long)cands[i].va, cands[i].span, cands[i].live,
             100.0 * cands[i].live / cands[i].span);
        for (int k = 0; k < 8; k++) {
            uint64_t e = p_.get<uint64_t>(cands[i].va + uint64_t(k) * 8);
            if (!e) { note("      [%d]  -", k); continue; }
            note("      [%d]  0x%llX %s", k, (unsigned long long)e,
                 live_entity(e) ? "live" : "(not an object)");
        }
    }
    if (!cands.empty()) arrays_ = cands;
    else note("\n    no container matched.  The histogram above is the diagnostic:");
    note("    if even the >= 16 bucket is empty, the entity array does not sit");
    note("    in writable memory the way this assumes.");
}

// -----------------------------------------------------------------------------
//  Stage 3 -- position field, anchored on the discovered entities.
// -----------------------------------------------------------------------------
void Discovery::stage3_position() {
    note("\n=== stage 3  position field ===");
    if (arrays_.empty()) {
        note("    no entity container from stage 2 -- cannot scope the search.");
        return;
    }

    std::vector<uint64_t> objs;
    for (const auto& a : arrays_) {
        for (int k = 0; k < a.span && k < 4096; k++) {
            uint64_t e = p_.get<uint64_t>(a.va + uint64_t(k) * 8);
            if (e) objs.push_back(e);
        }
        if (objs.size() >= 4000) break;
    }
    std::sort(objs.begin(), objs.end());
    objs.erase(std::unique(objs.begin(), objs.end()), objs.end());
    note("    %zu live entities", objs.size());
    if (objs.size() < 16) { note("    too few"); return; }

    constexpr size_t WIN = 0x300;
    std::unordered_map<uint32_t, int> tally;
    std::unordered_map<uint32_t, std::array<double, 3>> sum;
    std::unordered_map<uint32_t, float> lo, hi;
    std::vector<uint8_t> w(WIN);

    for (uint64_t e : objs) {
        if (!p_.read(e, w.data(), WIN)) continue;
        for (size_t o = 0; o + 12 <= WIN; o += 4) {
            float f[3];
            memcpy(f, w.data() + o, sizeof(f));
            if (!finite3(f)) continue;
            if (std::fabs(f[0]) > 100000.f || std::fabs(f[1]) > 100000.f ||
                std::fabs(f[2]) > 100000.f) continue;
            int nz = (std::fabs(f[0]) > 0.01f) + (std::fabs(f[1]) > 0.01f) +
                     (std::fabs(f[2]) > 0.01f);
            if (nz < 2) continue;
            uint32_t off = (uint32_t)o;
            tally[off]++;
            sum[off][0] += f[0]; sum[off][1] += f[1]; sum[off][2] += f[2];
            auto il = lo.find(off);
            if (il == lo.end()) { lo[off] = f[0]; hi[off] = f[0]; }
            il = lo.find(off);
            hi[off] = std::max(hi[off], f[0]);
            lo[off] = std::min(lo[off], f[0]);
        }
    }

    std::vector<PosCand> v;
    for (const auto& kv : tally)
        if (kv.second >= (int)(objs.size() / 4))
            v.push_back({ kv.first, kv.second,
                          (float)(sum[kv.first][0] / kv.second),
                          (float)(sum[kv.first][1] / kv.second),
                          (float)(sum[kv.first][2] / kv.second) });
    std::sort(v.begin(), v.end(), [](const PosCand& a, const PosCand& b) {
        return a.objects > b.objects;
    });

    note("\n    offset      entities   mean position");
    for (size_t i = 0; i < v.size() && i < 16; i++)
        note("    +0x%03X   %8d   %.1f, %.1f, %.1f", v[i].off, v[i].objects,
             v[i].mx, v[i].my, v[i].mz);
    if (v.empty())
        note("    no float3 appears in a quarter of the entities.  Either the");
        note("    positions are stored behind a pointer, or the 'entities' in");
        note("    stage 2 are not players.");
    pos_ = v;
}

// -----------------------------------------------------------------------------
//  Stage 4 -- health and team.
// -----------------------------------------------------------------------------
void Discovery::stage4_scalars() {
    note("\n=== stage 4  health and team ===");
    if (pos_.empty()) { note("    no position offset from stage 3"); return; }

    std::vector<uint64_t> objs;
    for (const auto& a : arrays_) {
        for (int k = 0; k < a.span && k < 4096; k++) {
            uint64_t e = p_.get<uint64_t>(a.va + uint64_t(k) * 8);
            if (e) objs.push_back(e);
        }
        if (objs.size() >= 2000) break;
    }
    std::sort(objs.begin(), objs.end());
    objs.erase(std::unique(objs.begin(), objs.end()), objs.end());
    if (objs.size() < 16) { note("    too few entities"); return; }
    note("    %zu entities, position at +0x%03X", objs.size(), pos_.front().off);

    constexpr size_t WIN = 0x200;
    std::vector<std::array<uint8_t, WIN>> s1(objs.size()), s2(objs.size());
    for (size_t i = 0; i < objs.size(); i++)
        if (!p_.read(objs[i], s1[i].data(), WIN)) s1[i].fill(0);

    note("    >>> MOVE, JUMP AND TAKE DAMAGE FOR THE NEXT 2 SECONDS <<<");
    fflush(stdout);
    Sleep(2000);
    for (size_t i = 0; i < objs.size(); i++)
        if (!p_.read(objs[i], s2[i].data(), WIN)) s2[i] = s1[i];

    note("\n    offset     int 0..100        changed        byte <= 4");
    for (size_t off = 0; off + 4 <= WIN; off += 4) {
        int inRange = 0, changed = 0, byteTeam = 0;
        for (size_t i = 0; i < objs.size(); i++) {
            int32_t iv; memcpy(&iv, s1[i].data() + off, 4);
            if (iv >= 0 && iv <= 100) inRange++;
            if (memcmp(s1[i].data() + off, s2[i].data() + off, 4) != 0) changed++;
            if (s1[i][off] <= 4) byteTeam++;
        }
        size_t n = objs.size();
        if (inRange * 2 < n) continue;
        if (changed * 8 < n && byteTeam * 2 < n) continue;
        note("    +0x%03X   %8zu/%-7zu %8zu/%-7zu %8zu/%zu", (uint32_t)off,
             (size_t)inRange, n, (size_t)changed, n, (size_t)byteTeam, n);
        scalars_.push_back({ (uint32_t)off, inRange, changed, byteTeam });
    }
    if (scalars_.empty())
        note("    no scalar in 0..100 across half the entities.");
}

void Discovery::write_json() {
    FILE* f = fopen("bs_probe.json", "w");
    if (!f) { note("[!] cannot write bs_probe.json"); return; }
    fprintf(f, "{\n  \"pid\": %u,\n  \"base\": \"0x%llX\",\n", p_.pid(),
            (unsigned long long)p_.base());
    fprintf(f, "  \"viewproj\": { \"found\": %s, \"va\": \"0x%llX\", \"fov\": %.2f, \"m\": [",
            vp_ok_ ? "true" : "false", (unsigned long long)vp_va_, vp_fov_);
    for (int k = 0; k < 16; k++) fprintf(f, "%s%.5f", k ? ", " : "", vp_[k]);
    fprintf(f, "] },\n");
    fprintf(f, "  \"entity_arrays\": [");
    for (size_t i = 0; i < arrays_.size(); i++)
        fprintf(f, "%s{ \"va\": \"0x%llX\", \"slots\": %d, \"live\": %d }",
                i ? ", " : "", (unsigned long long)arrays_[i].va,
                arrays_[i].span, arrays_[i].live);
    fprintf(f, "],\n");
    fprintf(f, "  \"position_offsets\": [");
    for (size_t i = 0; i < pos_.size(); i++)
        fprintf(f, "%s{ \"offset\": \"0x%X\", \"entities\": %d, \"mean\": [%.1f, %.1f, %.1f] }",
                i ? ", " : "", pos_[i].off, pos_[i].objects, pos_[i].mx, pos_[i].my, pos_[i].mz);
    fprintf(f, "],\n");
    fprintf(f, "  \"scalar_offsets\": [");
    for (size_t i = 0; i < scalars_.size(); i++)
        fprintf(f, "%s{ \"offset\": \"0x%X\", \"in_0_100\": %d, \"changed\": %d, \"byte_le_4\": %d }",
                i ? ", " : "", scalars_[i].off, scalars_[i].in_range,
                scalars_[i].changed, scalars_[i].byte_team);
    fprintf(f, "]\n}\n");
    fclose(f);
    note("\n[+] wrote bs_probe.json");
}

void Discovery::run() {
    all_ = p_.all_regions();
    regs_ = p_.rw_regions();
    stage0_modules();
    stage1_viewproj();
    stage2_entities();
    stage3_position();
    stage4_scalars();
    write_json();
}

} // namespace bs

// ====================================================================== main
int main(int argc, char** argv) {
    std::wstring module = L"BloodStrike.exe";

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--module") && i + 1 < argc) module = widen(argv[++i]);
        else if (!strcmp(argv[i], "--help")) {
            printf("bs_probe [--module BloodStrike.exe]\n");
            return 0;
        }
    }

    printf("bs_probe -- Blood Strike runtime discovery\n");
    printf("=============================================\n");

    bs::Proc proc;
    if (!proc.attach(module.c_str())) {
        printf("[X] could not attach to %ls\n\n", module.c_str());
        printf("    1. is the game running?\n");
        printf("    2. is this shell elevated?\n");
        printf("    3. is the process hidden by the anti-cheat?\n\n");
        printf("    If 1 and 2 hold, the read path itself is the problem and the");
        printf("\n    kernel backend in src/memory.cpp is the fallback.\n");
        return 2;
    }

    printf("[+] pid %u   base 0x%llX\n", proc.pid(), (unsigned long long)proc.base());
    for (const auto& s : proc.sections)
        printf("    %-9s rva 0x%08X  size 0x%07X   %c%c%c\n", s.name, s.rva, s.vsize,
               s.readable() ? 'r' : '-', s.exec() ? 'x' : '-',
               (s.chars & 0x80000000) ? 'w' : '-');

    bs::Discovery d(proc);
    d.run();
    return 0;
}
