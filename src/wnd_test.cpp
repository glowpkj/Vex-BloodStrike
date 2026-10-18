// -----------------------------------------------------------------------------
//  wnd_test.cpp  --  minimal reproduction of the overlay window crash
//
//  bs_esp crashes inside CreateWindowExW for the overlay's own window, before
//  ImGui has a context.  This isolates that single call so the cause can be
//  named instead of guessed at: it registers the class, creates the window with
//  the same extended style, and pumps messages, printing each step.
//
//  Deliberately no ImGui, no D3D, no game access.  If this crashes, the fault is
//  in the window creation itself; if it does not, the fault is in the
//  interaction between CreateWindowExW and the ImGui backend.
// -----------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdio>

#include "overlay.hpp"
#include "stealth.hpp"

namespace vex { namespace kmm { bool ready() { return false; } } }

/// Drive the real Overlay class against the console window, so the crash can be
/// reproduced without the game, without D3D-on-the-game, and without elevation
/// mattering.  Everything up to and including create_device() is exercised.
static void overlay_probe() {
    printf("[probe] --- real Overlay, targeting a window of our own ---\n");
    // A plain host window stands in for the game window: all create_on() needs
    // is a valid HWND to read a rectangle from.
    HWND host = CreateWindowExW(0, L"VexWndTest", L"", WS_OVERLAPPEDWINDOW,
                                0, 0, 640, 480, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    printf("[probe] host hwnd %p\n", (void*)host);
    if (!host) return;

    vex::Overlay ov;
    ov.set_cpu_present(true);            // exercise the UpdateLayeredWindow path
    bool ok = ov.create_on(host);
    printf("[probe] create_on -> %d, hwnd %p, ready %d, cpu_present %d\n",
           (int)ok, (void*)ov.hwnd(), (int)ov.ready(), (int)ov.cpu_present());
    if (ok) {
        for (int i = 0; i < 5; ++i) {
            ov.begin_frame();
            ov.end_frame();
        }
        printf("[probe] five frames drawn, last present %.1f ms\n",
               ov.last_present_ms());
    }
    ov.destroy();
    DestroyWindow(host);
    printf("[probe] destroyed cleanly\n");
}

static int g_created = 0, g_msgs = 0;

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ++g_msgs;
    return DefWindowProcW(h, m, w, l);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("[test] start\n");

    // bs_esp calls this before touching any window.  If it is what breaks
    // CreateWindowExW, this line is where the reproduction happens.
    vex::stealth::harden_process();
    printf("[test] harden_process returned\n");

    HMODULE self = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = proc;
    wc.hInstance     = self;
    wc.lpszClassName = L"VexWndTest";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        DWORD e = GetLastError();
        printf("[test] RegisterClassExW err %lu\n", e);
        if (e != 1410) return 1;
    }
    printf("[test] class ok\n");

    // Same style as the overlay.
    HWND h1 = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 800, 600,
        nullptr, nullptr, self, nullptr);
    printf("[test] layered+transparent window: %p (err %lu)\n",
           (void*)h1, h1 ? 0ul : GetLastError());
    if (h1) ++g_created;

    // Without WS_EX_LAYERED, to see whether that extended style is the trigger.
    HWND h2 = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 800, 600,
        nullptr, nullptr, self, nullptr);
    printf("[test] plain window: %p (err %lu)\n",
           (void*)h2, h2 ? 0ul : GetLastError());
    if (h2) ++g_created;

    for (int i = 0; i < 50 && g_msgs < 100; ++i) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
    printf("[test] pumped %d messages\n", g_msgs);

    if (h2) DestroyWindow(h2);
    if (h1) DestroyWindow(h1);

    overlay_probe();

    printf("[test] done, %d windows created\n", g_created);
    return 0;
}
