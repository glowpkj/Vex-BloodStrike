// -----------------------------------------------------------------------------
//  stealth.hpp  --  anti-detection layer
//
//  Three surfaces, because a kernel anticheat sees all three:
//
//    1. Driver side  (driver/protect.c, compiled into vexdrv.sys)
//       ObRegisterCallbacks strips VM_READ / VM_WRITE / CREATE_THREAD /
//       TERMINATE / DUP_HANDLE from any handle opened to the overlay process,
//       and hides the process from the system process list.  A scanner that
//       tries to open us gets a stripped handle rather than a clean failure.
//
//    2. Image        (this file)
//       Run from memory and delete the on-disk copy, so there is no module
//       path, no .exe in the directory, and nothing for a filesystem scanner
//       to find.
//
//    3. Runtime      (this file)
//       Hide from debuggers, keep threads out of the process snapshot, hold
//       the overlay window out of ordinary enumeration, and jitter the read
//       pattern so the memory access trace has no fixed cadence.
// -----------------------------------------------------------------------------
#pragma once

#include <Windows.h>
#include <string>

namespace vex {

namespace stealth {

/// Copy this executable into memory, run it from there, and mark the on-disk
/// copy for deletion.  Call before anything else in wWinMain.  Returns false
/// and falls back to running from disk if any step fails.
bool run_from_memory_and_self_delete(const wchar_t* self_path);

/// Clear BeingDebugged, hide the debugger thread from the process snapshot, and
/// ask the kernel to not report us to debuggers.
void harden_process();

/// Apply the thread context flag (thread hide-from-debugger) to the calling
/// thread.  Cheap enough to call once per frame.
void hide_thread();

/// Rename the process in Task Manager to something inert.
void disguise_process_name();

/// Overlay window: no owner in enumeration, no title, tool-window style so it
/// does not appear in the alt-tab list.
void harden_window(HWND hwnd);

/// Jittered sleep for the read loop, so the memory access trace does not have a
/// period an anti-cheat can key on.
void jittered_sleep(DWORD base_ms);

/// True once the driver confirmed the callbacks were registered.
bool kernel_protection_active();

} // namespace stealth
} // namespace vex
