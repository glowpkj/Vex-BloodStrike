// -----------------------------------------------------------------------------
//  chain_hunt.cpp  --  find the engine chain by shape, not by address.
//
//  Every source disagrees on where ClientEngine lives.  The dump says
//  0x7ECE2E8; the forum says 0x7FCE2E8 and 0x7FCD2E8; all three read back as
//  ASCII strings in this build.  Addresses move between patches, structure does
//  not, so search for the chain itself:
//
//      slot -> A        A+0x58 -> B        B+0x58 -> C
//
//  with A, B and C each pointing at a live object (first qword inside .text),
//  and C answering at +0x238 (camera) and +0x288 (actor).  A random qword has no
//  chance of satisfying five linked constraints at once.
//
//  Only the image's own writable sections are scanned: globals live there, and
//  sweeping the whole heap just surfaces other subsystems.
//
//  Written in C++ rather than PowerShell because the per-candidate check is a
//  ReadProcessMemory call and PowerShell's marshalling overhead made a sweep
//  that takes seconds take minutes.
// -----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace {

HANDLE g_h = nullptr;
uint64_t g_base = 0, g_tlo = 0, g_thi = 0;

inline bool read(uint64_t va, void* dst, size_t len) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(g_h, (LPCVOID)va, dst, len, &got)) return false;
    return got == len;
}

inline uint64_t u64(uint64_t va) {
    uint64_t v = 0;
    read(va, &v, sizeof v);
    return v;
}

/// A live C++ instance: its first qword is a vtable pointer into .text.
inline bool is_object(uint64_t va) {
    if (va < 0x10000 || va > 0x00007FFFFFFFFFFFull) return false;
    uint64_t vt = u64(va);
    return vt >= g_tlo && vt < g_thi;
}

struct Hit {
    uint64_t slot, a, b, c, cam, act;
    bool cam_ok, act_ok;
};

// Note: field order in the aggregate below must match, and the flags are set
// from the is_object() results rather than left to the implicit bool conversions
// of the pointer values.

} // namespace

int main() {
    printf("chain_hunt -- find the engine chain by structure\n");
    printf("===============================================\n");

    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) { printf("[X] process snapshot failed\n"); return 2; }
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(s, &pe)) {
        do { if (!_wcsicmp(pe.szExeFile, L"BloodStrike.exe")) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    if (!pid) { printf("[X] BloodStrike.exe not running\n"); return 2; }

    g_h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!g_h) {
        printf("[X] OpenProcess failed, error %lu\n", GetLastError());
        printf("    Run this as Administrator.\n");
        return 2;
    }

    HANDLE s2 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s2 != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof(me) };
        if (Module32FirstW(s2, &me)) {
            do { if (!_wcsicmp(me.szModule, L"BloodStrike.exe")) { g_base = (uint64_t)me.modBaseAddr; break; }
            } while (Module32NextW(s2, &me));
        }
        CloseHandle(s2);
    }
    if (!g_base) { printf("[X] module base not found\n"); return 2; }

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};

    // Section table, read from the on-disk image.  The in-memory headers are
    // scrubbed by the protector, which is why this cannot just read them from
    // the process.  Read a generous prefix: a packed image can put e_lfanew
    // further out than the usual 0x80.
    bool have_nt = false;
    {
        wchar_t path[MAX_PATH]{}; DWORD n = MAX_PATH;
        if (!QueryFullProcessImageNameW(g_h, 0, path, &n)) {
            printf("[X] QueryFullProcessImageName failed, error %lu\n", GetLastError());
            return 2;
        }
        printf("[+] image: %ls\n", path);

        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            printf("[X] cannot open the image file, error %lu\n", GetLastError());
            return 2;
        }
        std::vector<uint8_t> head(1u << 20);
        DWORD got = 0;
        if (!ReadFile(f, head.data(), (DWORD)head.size(), &got, nullptr) || got < 0x200) {
            printf("[X] short read on the image file\n");
            CloseHandle(f);
            return 2;
        }
        CloseHandle(f);

        auto* d = (IMAGE_DOS_HEADER*)head.data();
        printf("    e_magic 0x%04X   e_lfanew 0x%X\n", d->e_magic, d->e_lfanew);
        if (d->e_magic != IMAGE_DOS_SIGNATURE || (size_t)d->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > got) {
            printf("[X] bad DOS header or e_lfanew out of range\n");
            return 2;
        }
        auto* ntptr = (IMAGE_NT_HEADERS64*)(head.data() + d->e_lfanew);
        printf("    NT signature 0x%08X   sections %u\n",
               ntptr->Signature, ntptr->FileHeader.NumberOfSections);
        if (ntptr->Signature != IMAGE_NT_SIGNATURE) { printf("[X] bad NT signature\n"); return 2; }
        dos = *d;
        nt = *ntptr;
        have_nt = true;
    }
    if (!have_nt) return 2;
    const IMAGE_SECTION_HEADER* sh =
        (const IMAGE_SECTION_HEADER*)((const uint8_t*)&nt + sizeof(IMAGE_NT_HEADERS64));

    uint64_t scan_lo = 0, scan_hi = 0;
    for (uint16_t i = 0; i < nt.FileHeader.NumberOfSections; ++i, ++sh) {
        char nm[9]{};
        memcpy(nm, sh->Name, 8);
        uint64_t rva = sh->VirtualAddress;
        uint64_t vsize = (sh->Misc.VirtualSize > sh->SizeOfRawData)
                         ? sh->Misc.VirtualSize : sh->SizeOfRawData;
        if (!strcmp(nm, ".text")) { g_tlo = g_base + rva; g_thi = g_base + rva + vsize; }
        if (!strcmp(nm, ".data")) { scan_lo = g_base + rva; scan_hi = g_base + rva + vsize; }
    }
    if (!g_tlo || !scan_lo) { printf("[X] could not map sections\n"); return 2; }

    printf("[+] pid %u  base 0x%llX\n", pid, (unsigned long long)g_base);
    printf("    .text  0x%llX .. 0x%llX\n", (unsigned long long)g_tlo, (unsigned long long)g_thi);
    printf("    scan   0x%llX .. 0x%llX  (%.1f MB)\n", (unsigned long long)scan_lo,
           (unsigned long long)scan_hi, (scan_hi - scan_lo) / 1048576.0);

    // Sanity-check the handle before spending minutes scanning.  A stale
    // handle from a game that has since exited is the most likely reason a run
    // produces nothing, and saying so up front beats an empty report.
    {
        uint64_t probe = 0;
        if (!read(g_base, &probe, 8) || probe == 0) {
            printf("\n[X] the handle is valid but reads return nothing.");
            printf("\n    The game most likely exited between listing and reading.");
            printf("\n    Re-run while BloodStrike.exe is running.\n");
            return 3;
        }
        printf("    handle check    ok\n");
    }

    std::vector<Hit> hits;
    constexpr size_t CH = 8u << 20;
    std::vector<uint8_t> buf(CH);
    uint64_t scanned = 0;
    const uint64_t deadline = GetTickCount64() + 180'000;

    for (uint64_t a = scan_lo; a < scan_hi; a += CH) {
        if (GetTickCount64() > deadline) { printf("[!] time limit\n"); break; }
        size_t want = (size_t)((scan_hi - a) < CH ? (scan_hi - a) : CH);
        if (!read(a, buf.data(), want)) continue;
        scanned += want;

        for (size_t i = 0; i + 8 <= want; i += 8) {
            uint64_t A;
            memcpy(&A, buf.data() + i, 8);
            if (!is_object(A)) continue;
            uint64_t B = u64(A + 0x58);
            if (!is_object(B)) continue;
            uint64_t C = u64(B + 0x58);
            if (!is_object(C)) continue;
            uint64_t cam = u64(C + 0x238);
            uint64_t act = u64(C + 0x288);
            bool camOk = is_object(cam), actOk = is_object(act);
            if (!camOk && !actOk) continue;
            hits.push_back({ a + i, A, B, C, cam, act, camOk, actOk });
        }
    }

    printf("\n[+] bytes scanned : %llu MB\n", (unsigned long long)(scanned >> 20));
    printf("[+] chain hits    : %zu\n\n", hits.size());

    int shown = 0;
    for (const auto& h : hits) {
        if (shown++ >= 20) { printf("    ... %zu more\n", hits.size() - 20); break; }
        printf("  slot 0x%llX   rva 0x%llX\n",
               (unsigned long long)h.slot, (unsigned long long)(h.slot - g_base));
        printf("      A            0x%llX\n",  (unsigned long long)h.a);
        printf("      A+0x58 -> B  0x%llX\n",  (unsigned long long)h.b);
        printf("      B+0x58 -> C  0x%llX\n",  (unsigned long long)h.c);
        printf("      C+0x238 cam  0x%llX  %s\n", (unsigned long long)h.cam, h.cam_ok ? "OBJECT" : "-");
        printf("      C+0x288 act  0x%llX  %s\n", (unsigned long long)h.act, h.act_ok ? "OBJECT" : "-");
        printf("\n");
    }

    // Machine-readable so the ESP client can consume it directly.
    FILE* f = fopen("chain_hunt.json", "w");
    if (f) {
        fprintf(f, "{\n  \"base\": \"0x%llX\",\n  \"hits\": [", (unsigned long long)g_base);
        for (size_t i = 0; i < hits.size(); i++) {
            fprintf(f, "%s\n    { \"slot_rva\": \"0x%llX\", \"A\": \"0x%llX\", "
                       "\"B\": \"0x%llX\", \"C\": \"0x%llX\", \"cam\": \"0x%llX\", "
                       "\"act\": \"0x%llX\", \"cam_ok\": %s, \"act_ok\": %s }",
                    i ? "," : "",
                    (unsigned long long)(hits[i].slot - g_base),
                    (unsigned long long)hits[i].a, (unsigned long long)hits[i].b,
                    (unsigned long long)hits[i].c, (unsigned long long)hits[i].cam,
                    (unsigned long long)hits[i].act,
                    hits[i].cam_ok ? "true" : "false", hits[i].act_ok ? "true" : "false");
        }
        fprintf(f, "\n  ]\n}\n");
        fclose(f);
        printf("[+] wrote chain_hunt.json\n");
    }
    return 0;
}
