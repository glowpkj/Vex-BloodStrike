// =============================================================================
//  stealth.cpp  --  image, process and window hardening
//
//  Ground rule for this file: none of it is worth crashing over.
//
//  Everything here is cosmetic anti-inspection dressing.  BeingDebugged,
//  NtGlobalFlag and the process-name rewrite only matter if someone deliberately
//  attaches a debugger or opens our process -- and the thread-hiding call that
//  used to be here was fatal, because NtSetInformationThread enters the kernel
//  through ntdll and a kernel anticheat is free to make that call fault.
// =============================================================================
#include "stealth.hpp"

#include <winternl.h>
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

// -----------------------------------------------------------------------------
//  The kernel-mapped driver reports whether it is live through this symbol.
//  It is declared here and defined by the target that actually owns a driver
//  (bs_esp.cpp supplies a stub returning false, because this build reads through
//  a plain usermode handle).  Keeping the declaration local means this file has
//  no link-time dependency on the driver loader.
// -----------------------------------------------------------------------------
namespace vex { namespace kmm { bool ready(); } }

namespace vex::stealth {

namespace {

// ---- runtime-resolved so the IAT carries no suspicious imports ----------
typedef NTSTATUS (NTAPI *PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID,
                                                        ULONG, PULONG);
typedef NTSTATUS (NTAPI *PFN_RtlAdjustPrivilege)(ULONG, BOOLEAN, BOOLEAN*);

PFN_RtlAdjustPrivilege pfn_Privilege = nullptr;

constexpr ULONG SeDebugPrivilegeId = 20;

HMODULE g_ntdll = nullptr;

void resolve() {
    if (g_ntdll) return;
    g_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!g_ntdll) return;
    pfn_Privilege = (PFN_RtlAdjustPrivilege)
        GetProcAddress(g_ntdll, "RtlAdjustPrivilege");
}

// ---- PEB access -----------------------------------------------------------
//
// GetPEB() is not declared under WIN32_LEAN_AND_MEAN on every SDK version, and
// _PEB::NtGlobalFlag is not exposed by every one either.  The fields we touch sit
// at fixed x64 offsets and never move, so they are read and written directly.
// This compiles against any SDK and cannot break on a Windows SDK update.
constexpr uint64_t kPebBeingDebugged = 0x02;
constexpr uint64_t kPebProcessParams = 0x20;

/// 0xBC, not 0x68.  On x64 the PEB lays NtGlobalFlag out well after
/// ProcessParameters; 0x68 lands inside the process-heap / FastPebLock region,
/// and zeroing four bytes there corrupts a lock the loader and user32 take
/// during CreateWindowExW.  The symptom was a window that could not be created
/// in an elevated process -- exit code 0xC000041D, STATUS_FATAL_USER_CALLBACK_
/// EXCEPTION, i.e. a fault inside a user32 callback, with no printf either side
/// of the CreateWindowExW call ever appearing.  Not a window bug at all: a PEB
/// bug that only showed up when something asked the PEB for a lock.
constexpr uint64_t kPebNtGlobalFlag   = 0xBC;

constexpr uint64_t kPpImagePathName   = 0x60;
constexpr uint64_t kPpCommandLine     = 0x70;

/// The PEB only holds usermode pointers, so anything outside the 48-bit user
/// range is garbage.  Without this bound a bad ProcessParameters lands in kernel
/// space and writing through it faults there.
constexpr uint64_t kUserMax = 0x00007FFFFFFFFFFFull;

/// gs:[0x60] is the PEB.  Validates the result so a caller can bail rather than
/// fault.
uint8_t* peb_bytes() {
    const uint64_t peb = __readgsqword(0x60);
    if (peb < 0x10000 || peb > kUserMax) return nullptr;
    return (uint8_t*)(uintptr_t)peb;
}

// ---- cross-process reads (run-from-memory helper) -------------------------
bool remote_read(HANDLE proc, uint64_t addr, void* dst, size_t size) {
    SIZE_T got = 0;
    return ReadProcessMemory(proc, (LPCVOID)addr, dst, size, &got) && got == size;
}

/// PEB -> Ldr -> InLoadOrderModuleList -> first entry's DllBase.  The module a
/// process was just started from is always first in that list.
bool remote_image_base(HANDLE proc, uint64_t* out) {
    auto qip = (PFN_NtQueryInformationProcess)
        GetProcAddress(g_ntdll, "NtQueryInformationProcess");
    if (!qip) return false;

    uint64_t peb = 0;
    if (qip(proc, 0 /*ProcessBasicInformation*/, &peb, sizeof(peb), nullptr) < 0)
        return false;
    if (!peb) return false;

    uint64_t ldr = 0;
    if (!remote_read(proc, peb + 0x18 /*PEB::Ldr*/, &ldr, 8) || !ldr) return false;

    uint64_t entry = 0;                    // Flink of InLoadOrderModuleList
    if (!remote_read(proc, ldr + 0x20, &entry, 8) || !entry) return false;

    uint64_t base = 0;                     // LDR_DATA_TABLE_ENTRY::DllBase
    if (!remote_read(proc, entry + 0x30, &base, 8) || !base) return false;

    *out = base;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
//  1. run from memory + self delete
//
//  The child is launched from disk so the PE loader does the relocation and
//  import work, then suspended, then its image is overwritten with our own
//  in-memory copy before the first instruction runs.  What executes is therefore
//  not what was ever on disk.
// ---------------------------------------------------------------------------
bool run_from_memory_and_self_delete(const wchar_t* self_path) {
    resolve();
    if (!g_ntdll) return false;

    HANDLE f = CreateFileW(self_path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (128ll << 20)) {
        CloseHandle(f);
        return false;
    }

    std::vector<uint8_t> image((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!ReadFile(f, image.data(), (DWORD)image.size(), &got, nullptr) ||
        got != image.size()) {
        CloseHandle(f);
        return false;
    }
    CloseHandle(f);

    // We are about to overwrite a PE with a PE; make sure we have one.
    if (image[0] != 'M' || image[1] != 'Z') return false;

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(self_path, nullptr, nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED, nullptr, nullptr, &si, &pi))
        return false;

    HANDLE proc   = pi.hProcess;
    HANDLE thread = pi.hThread;

    uint64_t remote_base = 0;
    const bool found = remote_image_base(proc, &remote_base);

    bool ok = false;
    if (found) {
        SIZE_T wrote = 0;
        ok = WriteProcessMemory(proc, (void*)(uintptr_t)remote_base, image.data(),
                                image.size(), &wrote) &&
             wrote == image.size() &&
             FlushInstructionCache(proc, (void*)(uintptr_t)remote_base, image.size());
    }

    ResumeThread(thread);
    CloseHandle(thread);

    if (!ok) {
        // Nothing was overwritten, so the child is running the on-disk copy.
        // Terminate it rather than leave a duplicate instance behind.
        TerminateProcess(proc, 1);
        CloseHandle(proc);
        return false;
    }
    CloseHandle(proc);

    // The child's image section still references the file, so an immediate
    // delete usually fails with a sharing violation.  Schedule it for reboot;
    // the memory-image protection above is the part that matters either way.
    if (!DeleteFileW(self_path))
        MoveFileExW(self_path, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    SetEnvironmentVariableW(L"VEX_UNPACKED", L"1");
    return true;
}

// ---------------------------------------------------------------------------
//  2. process hardening
//
//  Cosmetic only, and fully fault-contained.  Nothing in here may abort the
//  process: the program has a job to do and none of this is that job.
// ---------------------------------------------------------------------------
void harden_process() {
    __try {
        resolve();
        if (!g_ntdll) return;

        uint8_t* peb = peb_bytes();
        if (peb) {
            // BeingDebugged is a single documented byte at 0x02; zeroing it is
            // always safe.
            *(uint8_t*)(peb + kPebBeingDebugged) = 0;

            // NtGlobalFlag is written conditionally, and that condition is the
            // fix for the crash described above.  A stale offset must never be
            // written blind: the previous value read a plausible-looking word
            // out of the middle of the PEB and zeroed it, and the process died
            // hours of debugging later, inside user32.  NtGlobalFlag only ever
            // carries the heap-debugging bits (0x40000000, 0x4000000, 0x400000,
            // 0x40000, 0x4000, 0x400, 0x10) and is zero on a normally-launched
            // process, so accept it only if it is a subset of that mask.  If the
            // read does not look like NtGlobalFlag, the offset is not one, and
            // the right move is to leave the memory alone.
            constexpr uint32_t kNtGlobalFlagMask = 0x40000470u;
            uint32_t* flag = (uint32_t*)(peb + kPebNtGlobalFlag);
            uint32_t  val  = *flag;
            if ((val & ~kNtGlobalFlagMask) == 0)
                *flag = val & ~kNtGlobalFlagMask;
        }

        // SeDebugPrivilege is what other processes use to inspect us, so drop it
        // once startup is done rather than holding it for our lifetime.
        if (pfn_Privilege) {
            BOOLEAN was = FALSE;
            pfn_Privilege(SeDebugPrivilegeId, TRUE,  &was);
            pfn_Privilege(SeDebugPrivilegeId, FALSE, &was);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Cosmetic only; the program must continue regardless.
    }
}

// Intentionally does nothing.
//
// The previous implementation called NtSetInformationThread with
// ThreadHideFromDebugger here.  That was the crash: RIP landed inside ntdll on a
// WRITE, i.e. the call went kernel-side and faulted, with RAX holding
// STATUS_INVALID_INFO_CLASS.  Beyond the crash, calling a kernel-observable
// thread API on every rendered frame is a behaviour worth not having anyway.
void hide_thread() { }

// Rewriting the image path in the PEB changes what Task Manager shows.  Both
// UNICODE_STRINGs are replaced in place, so nothing has to be freed.
void disguise_process_name() {
    __try {
        uint8_t* peb = peb_bytes();
        if (!peb) return;

        const uint64_t params = *(uint64_t*)(peb + kPebProcessParams);
        if (params < 0x10000 || params > kUserMax) return;

        uint8_t* pbase = (uint8_t*)(uintptr_t)params;

        static const wchar_t kFakePath[] = L"C:\\Windows\\System32\\RuntimeBroker.exe";
        static const wchar_t kFakeCmd[]  = L"\"C:\\Windows\\System32\\RuntimeBroker.exe\"";

        auto retarget = [](uint8_t* base, uint64_t field,
                           const wchar_t* text, size_t chars) {
            UNICODE_STRING* us = (UNICODE_STRING*)(base + field);
            if (!us->Buffer) return;
            us->Buffer        = (PWSTR)text;
            us->Length        = (USHORT)(chars * sizeof(wchar_t));
            us->MaximumLength = (USHORT)((chars + 1) * sizeof(wchar_t));
        };

        retarget(pbase, kPpImagePathName, kFakePath,
                 (sizeof(kFakePath) / sizeof(wchar_t)) - 1);
        retarget(pbase, kPpCommandLine, kFakeCmd,
                 (sizeof(kFakeCmd) / sizeof(wchar_t)) - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Cosmetic only; failing here changes nothing functionally.
    }
}

// WS_EX_TOOLWINDOW keeps the overlay out of the alt-tab list, WS_EX_NOACTIVATE
// keeps focus in the game, WS_EX_TRANSPARENT means no mouse reaches us.
void harden_window(HWND hwnd) {
    if (!hwnd) return;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT;
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    // An empty title means EnumWindows-based finders have nothing to match on.
    SetWindowTextW(hwnd, L"");
}

// ---------------------------------------------------------------------------
//  jittered sleep
//
//  Uniform in [0.75x, 1.25x].  The point is not to hide the loop but to stop the
//  memory access pattern having a period something can key on.
// ---------------------------------------------------------------------------
void jittered_sleep(DWORD base_ms) {
    if (base_ms < 4) { Sleep(base_ms); return; }

    const DWORD jitter = base_ms / 4;
    static std::mt19937 rng{ (unsigned)GetTickCount() };
    std::uniform_int_distribution<DWORD> dist(base_ms - jitter, base_ms + jitter);
    Sleep(dist(rng));
}

// The callbacks are registered from DriverEntry on the first IOCTL; if the
// device is open, they went in.
bool kernel_protection_active() {
    return kmm::ready();
}

} // namespace vex::stealth
