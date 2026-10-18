// =============================================================================
//  overlay.cpp
//
//  A WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE window
//  that sits over the game, owns a DX11 swapchain and draws with ImGui.  Because
//  the window never takes focus and forwards no mouse, the game keeps input.
// =============================================================================
#include "overlay.hpp"

#include <tchar.h>
#include <algorithm>
#include <dwmapi.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace vex {

namespace {

D3D_FEATURE_LEVEL g_feature_level;

} // namespace

Overlay::~Overlay() { destroy(); }

LRESULT CALLBACK Overlay::wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // -----------------------------------------------------------------------
    //  Click-through, guaranteed rather than requested.
    //
    //  The window is created with WS_EX_TRANSPARENT, which is the documented
    //  way to ask for a window that the mouse passes over.  It was not enough.
    //  The reported symptom is unambiguous and severe: while the overlay runs,
    //  the pointer cannot interact with ANYTHING on the system -- not the game,
    //  not other applications, not even Task Manager -- while the keyboard and
    //  focus continue to work.  Terminating the overlay restores the pointer
    //  instantly, and a build with no window at all never shows the problem.
    //  So the window is the cause, and the style alone does not prevent it.
    //
    //  WS_EX_TRANSPARENT is advisory: it is a hint honoured by the default
    //  hit-testing, and it is unreliable when the window is topmost, covers the
    //  screen, and has a DWM-extended frame (the -1 margins that give per-pixel
    //  alpha).  The documented, binding way to exclude a window from hit-testing
    //  is to answer WM_NCHITTEST with HTTRANSPARENT -- the window procedure
    //  itself says "this point is not mine, pass it to the window underneath",
    //  and neither the DWM nor the window manager can override that answer.
    //
    //  The same reasoning applies to the other three messages:
    //
    //    WM_SETCURSOR      the window must never set the cursor.  Returning TRUE
    //                      without calling SetCursor leaves whatever the window
    //                      underneath put there -- which is the game's clipped
    //                      crosshair cursor, or the normal arrow elsewhere.
    //    WM_MOUSEACTIVATE  a click on this window must never activate it.
    //                      MA_NOACTIVATE says so explicitly; without it the
    //                      system may raise or activate on click.
    //    WM_NCACTIVATE     the non-client activation state is refused, so the
    //                      window cannot become active through its frame.
    //
    //  Every one of these is an explicit refusal.  None of them has any effect
    //  on whether the overlay is drawn -- drawing is the swapchain's business,
    //  and DWM per-pixel alpha keeps working exactly as before.
    // -----------------------------------------------------------------------
    switch (m) {
    case WM_NCHITTEST:
        // Do not even let ImGui see it: the answer is fixed.
        return HTTRANSPARENT;

    case WM_SETCURSOR:
        return TRUE;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_NCACTIVATE:
        // Claim we handled it, without doing anything.  Passing it to
        // DefWindowProc would let the window repaint its frame for the new
        // activation state.
        return TRUE;

    default:
        break;
    }

    // The ImGui backend dereferences ImGui::GetCurrentContext() unconditionally,
    // and CreateWindowExW delivers WM_CREATE / WM_SIZE / WM_GETMINMAXINFO before
    // ImGui::CreateContext() has ever been called.  Without this guard the first
    // message crashes the overlay with an access violation.
    if (ImGui::GetCurrentContext() != nullptr) {
        if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

bool Overlay::create(const std::wstring& target_title) {
    target_ = FindWindowW(L"UnityWndClass", nullptr);
    if (!target_) {
        // Fall back to a title match when the class name is not found.
        target_ = FindWindowW(nullptr, target_title.c_str());
    }
    if (!target_) return false;
    if (!create_window())     return false;
    if (!create_device())     return false;
    if (!sync_to_target())    return false;
    return true;
}

bool Overlay::create_on(HWND target) {
    if (!target || !IsWindow(target)) {
        printf("[overlay] target handle invalid\n");
        return false;
    }
    target_ = target;
    if (!create_window()) {
        printf("[overlay] create_window failed, err %lu\n", GetLastError());
        return false;
    }
    printf("[overlay] window created, hwnd %p\n", (void*)hwnd_);
    if (!create_device()) {
        printf("[overlay] create_device failed\n");
        return false;
    }
    printf("[overlay] device created\n");
    if (!sync_to_target()) {
        RECT r{};
        BOOL got = GetWindowRect(target_, &r);
        printf("[overlay] sync_to_target failed: GetWindowRect=%d rect %ld,%ld,%ld,%ld\n",
               got, r.left, r.top, r.right, r.bottom);
        return false;
    }
    printf("[overlay] synced to %ld,%ld %ldx%ld\n",
           target_rect_.left, target_rect_.top,
           target_rect_.right - target_rect_.left,
           target_rect_.bottom - target_rect_.top);
    return true;
}

bool Overlay::create_window() {
    // The window class has to be registered against the module that owns the
    // window procedure.  The original code registered it against user32.dll,
    // which is the module that *implements* DefWindowProc but not the one that
    // owns Overlay::wnd_proc -- and CreateWindowExW then failed with 1407
    // (ERROR_CANNOT_FIND_WND_CLASS) because no class by that name existed in
    // the caller's context.  Registering against our own module is both correct
    // and what makes CreateWindowExW resolve the class.
    printf("[overlay] create_window enter\n");
    HMODULE self = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = Overlay::wnd_proc;
    wc.hInstance     = self;
    wc.lpszClassName  = L"VexOverlayWnd";
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;

    printf("[overlay] registering class on module %p\n", (void*)self);
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        // 1410 is ERROR_CLASS_ALREADY_EXISTS, which is fine: a previous overlay
        // in this session registered it, and the class is reusable.
        if (err != 1410) {
            printf("[overlay] RegisterClassExW failed, err %lu\n", err);
            return false;
        }
    }

    printf("[overlay] class registered, creating window\n");

    // -----------------------------------------------------------------------
    //  Two different extended styles, one per present path.
    //
    //  LAYERED_CPU (--ulw):
    //      LAYERED + TRANSPARENT + NOACTIVATE + TOOLWINDOW + TOPMOST.  No
    //      NOREDIRECTIONBITMAP -- that flag requires a composition swapchain and
    //      is what stopped the first Present from ever completing on the last
    //      attempt.  No DwmExtendFrameIntoClientArea either: on this path the
    //      transparency is supplied per-pixel by the BGRA buffer handed to
    //      UpdateLayeredWindow, and asking the DWM to extend the frame as well
    //      would fight it.
    //
    //  SWAPCHAIN (default):
    //      No LAYERED.  The DWM frame extension gives per-pixel alpha and the
    //      swapchain is composited by the DWM.  This path draws reliably.
    // -----------------------------------------------------------------------
    __try {
        if (cpu_present_) {
            hwnd_ = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                wc.lpszClassName, L"", WS_POPUP,
                0, 0, 1920, 1080,
                nullptr, nullptr, self, nullptr);
        } else {
            hwnd_ = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TRANSPARENT |
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                wc.lpszClassName, L"", WS_POPUP,
                0, 0, 1920, 1080,
                nullptr, nullptr, self, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("[overlay] CreateWindowExW raised 0x%08lX\n", GetExceptionCode());
        return false;
    }
    printf("[overlay] CreateWindowExW returned %p\n", (void*)hwnd_);

    if (!hwnd_) {
        printf("[overlay] CreateWindowExW failed, err %lu\n", GetLastError());
        return false;
    }

    if (cpu_present_) {
        printf("[overlay] present path: LAYERED_CPU (UpdateLayeredWindow); "
               "styles LAYERED|TRANSPARENT|NOACTIVATE|TOOLWINDOW|TOPMOST; "
               "no DWM frame extension\n");
        return true;
    }

    // -----------------------------------------------------------------------
    //  WS_EX_LAYERED is REQUIRED, and SetLayeredWindowAttributes is FORBIDDEN.
    //
    //  These two were confused for a long time, and the confusion cost the
    //  mouse.  They are separate things:
    //
    //    WS_EX_LAYERED is a STYLE.  Among other things it is the flag that
    //    puts the window on the input-transparent path: without it, the
    //    documented mouse pass-through of WS_EX_TRANSPARENT is only guaranteed
    //    for windows on the same input thread, which an overlay over another
    //    process's game is not.  The measured symptom matches that exactly --
    //    the pointer MOVES normally (so nothing is captured or clipped) but
    //    clicks are delivered to the overlay and swallowed, on every window on
    //    the desktop, from the moment the overlay exists.
    //
    //    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA) is an ATTRIBUTE.
    //    It applies ONE alpha to the whole window, overriding the per-pixel
    //    alpha from the DWM frame extension.  With 255 the window is opaque,
    //    every pixel including the ones cleared to (0,0,0,0) -- the black
    //    screen.  Calling it is what made earlier "add WS_EX_LAYERED" attempts
    //    fail, and it is why WS_EX_LAYERED got the blame for a problem that was
    //    always the attribute.
    //
    //  So: the style is set, the attribute is never called.  But setting
    //  WS_EX_LAYERED alone is not enough either, and the run that proved the
    //  mouse was fixed by it also proved the drawing had disappeared: a layered
    //  window with no SetLayeredWindowAttributes and no UpdateLayeredWindow has
    //  no defined content of its own, and it composites as fully transparent --
    //  the overlay produced a live process, thirteen tracked actors, and nothing
    //  on screen.
    //
    //  WS_EX_NOREDIRECTIONBITMAP is what makes the two coexist.  It tells the
    //  DWM not to allocate a redirection bitmap for this window, so the window
    //  is composited straight from the swapchain instead of from a bitmap that
    //  the layering code owns.  That is the documented arrangement for a D3D
    //  window that is layered: the swapchain's own alpha is what is composited,
    //  the DWM frame extension still supplies per-pixel alpha, and the input
    //  pass-through from WS_EX_LAYERED is retained.
    //
    //  All five flags are now load-bearing and none can be dropped:
    //    LAYERED              input pass-through across processes
    //    NOREDIRECTIONBITMAP  draw from the swapchain, not from a null bitmap
    //    TRANSPARENT          mouse passes over
    //    NOACTIVATE           never takes focus
    //    TOOLWINDOW           no taskbar entry
    //    TOPMOST              above the game
    // -----------------------------------------------------------------------
    DWORD ex = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
               WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP);

    MARGINS m = { -1, -1, -1, -1 };
    HRESULT dm = DwmExtendFrameIntoClientArea(hwnd_, &m);
    printf("[overlay] DwmExtendFrameIntoClientArea hr 0x%08lX %s\n",
           (unsigned long)dm,
           SUCCEEDED(dm) ? "(per-pixel alpha enabled)"
                         : "(FAILED -- window will be opaque)");

    // The window is layered and transparent and never activated; on top of the
    // style, the window procedure answers WM_NCHITTEST with HTTRANSPARENT, so
    // the hit-test is refused twice over and by two different mechanisms.
    printf("[overlay] styles: LAYERED|NOREDIRECTIONBITMAP|TRANSPARENT|NOACTIVATE|"
           "TOOLWINDOW|TOPMOST; no SetLayeredWindowAttributes (per-pixel alpha stays)\n");

    // Deliberately NOT shown here.  ShowWindow(SW_SHOWNOACTIVATE) at creation
    // time is what put an empty topmost window on the desktop the instant the
    // program started -- before a single frame had been drawn and before anyone
    // had decided it should be visible.  A layered window with an un-presented
    // backbuffer is a black rectangle, and because the window is click-through
    // and topmost there was no way to dismiss it.
    //
    // Visibility is now owned entirely by the caller, which shows the window
    // only while the game is the foreground window and hides it otherwise.
    // CreateWindowExW without WS_VISIBLE already leaves it hidden, so this is
    // consistent with the window's own state.
    return true;
}

bool Overlay::create_device() {
    // Sizes come from the window that was just created, not from a constant.
    // DXGI_ERROR_INVALID_CALL (0x887A0001) is what a swapchain desc that does
    // not match its output window produces, and the previous version hard-coded
    // 1920x1080 with BufferCount left at zero.
    RECT cr{ 0, 0, 1920, 1080 };
    if (hwnd_) GetClientRect(hwnd_, &cr);

    if (cpu_present_) {
        // -----------------------------------------------------------------
        //  LAYERED_CPU: no swapchain at all.
        //
        //  A device is still needed to draw with ImGui/D3D11, but nothing is
        //  presented through DXGI.  The frame is rendered into an ordinary
        //  offscreen texture and read back.
        // -----------------------------------------------------------------
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, (UINT)std::size(levels), D3D11_SDK_VERSION,
            &device_, &g_feature_level, &context_);

        if (hr == DXGI_ERROR_UNSUPPORTED)
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                levels, (UINT)std::size(levels), D3D11_SDK_VERSION,
                &device_, &g_feature_level, &context_);

        if (FAILED(hr)) {
            printf("[overlay] D3D11CreateDevice failed, hr 0x%08lX\n", (unsigned long)hr);
            destroy();
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename  = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
        ImGui_ImplWin32_Init(hwnd_);
        ImGui_ImplDX11_Init(device_, context_);
        imgui_ready_ = true;

        if (!create_cpu_present()) return false;
        return true;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Width  = cr.right - cr.left;
    sd.BufferDesc.Height = cr.bottom - cr.top;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 240;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.BufferCount  = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd_;
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags        = 0;

    printf("[overlay] swapchain desc %lux%lu on hwnd %p\n",
           (unsigned long)sd.BufferDesc.Width,
           (unsigned long)sd.BufferDesc.Height, (void*)hwnd_);

    UINT flags = 0;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, (UINT)std::size(levels), D3D11_SDK_VERSION,
        &sd, &swapchain_, &device_, &g_feature_level, &context_);

    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, (UINT)std::size(levels), D3D11_SDK_VERSION,
            &sd, &swapchain_, &device_, &g_feature_level, &context_);

    if (FAILED(hr)) {
        // Report the HRESULT.  A silent false here is indistinguishable from a
        // dozen other failures, and the first version of this cost two runs
        // because it printed nothing.
        printf("[overlay] D3D11CreateDeviceAndSwapChain failed, hr 0x%08lX\n",
               (unsigned long)hr);
        destroy();
        return false;
    }

    ID3D11Texture2D* back = nullptr;
    HRESULT hb = swapchain_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hb)) {
        printf("[overlay] swapchain GetBuffer failed, hr 0x%08lX\n", (unsigned long)hb);
        destroy();
        return false;
    }
    device_->CreateRenderTargetView(back, nullptr, &rtv_);
    back->Release();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename  = nullptr;

    // -----------------------------------------------------------------------
    //  Every ImGui input feature that touches the system cursor is turned off.
    //
    //  Auditing the backend rather than assuming: imgui_impl_win32.cpp contains
    //  real capture and cursor code -- it calls ::SetCapture on WM_LBUTTONDOWN,
    //  ::ReleaseCapture on button-up, and SetCursor while a resize or a text
    //  selection is in progress.  None of those is needed here, because this
    //  window has no interactive widgets outside the menu, and all of them are
    //  exactly the class of operation that was leaving the pointer stuck: a
    //  window that takes the capture keeps the mouse until it releases it.
    //
    //  NoMouseCursorChange stops the backend calling SetCursor and
    //  ShowCursor, so the arrow never changes shape because of this window.
    //  NoMouse is stronger and is what actually matters: with it set, the
    //  backend does not process mouse input at all, so the SetCapture path is
    //  never reached -- not because the messages do not arrive (WM_NCHITTEST
    //  already sees to that) but because the backend ignores them even if they
    //  somehow do.
    //
    //  The menu is still clickable because this project never relied on the
    //  backend for it: the menu is opened and closed with the keyboard, and the
    //  widgets inside it are driven by keys as well.  If mouse interaction with
    //  the menu is ever wanted back, the correct fix is a separate child window
    //  for the menu, not re-enabling capture on the overlay.
    // -----------------------------------------------------------------------
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;

    ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(device_, context_);
    imgui_ready_ = true;
    return true;
}

bool Overlay::create_cpu_present() {
    // -----------------------------------------------------------------------
    //  The offscreen texture the frame is drawn into, and the staging texture
    //  it is read back through.
    //
    //  offscreen_       BGRA8 render target.  BGRA rather than RGBA because
    //                   UpdateLayeredWindow's BLENDFUNCTION expects
    //                   AC_SRC_ALPHA with premultiplied BGRA, and matching the
    //                   channel order here avoids a swap on every pixel.
    //  staging_         CPU-readable mirror of it.  D3D11 requires a separate
    //                   texture with CPU_ACCESS_READ; a render target can never
    //                   be mapped directly.
    // -----------------------------------------------------------------------
    RECT cr{ 0, 0, 1920, 1080 };
    if (hwnd_) GetClientRect(hwnd_, &cr);
    present_w_ = cr.right - cr.left;
    present_h_ = cr.bottom - cr.top;
    if (present_w_ <= 0 || present_h_ <= 0) { present_w_ = 1920; present_h_ = 1080; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)present_w_;
    td.Height           = (UINT)present_h_;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &offscreen_))) {
        printf("[overlay] offscreen CreateTexture2D failed\n");
        destroy();
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(offscreen_, nullptr, &offscreen_rtv_))) {
        printf("[overlay] offscreen CreateRenderTargetView failed\n");
        destroy();
        return false;
    }

    D3D11_TEXTURE2D_DESC sd2 = td;
    sd2.Usage          = D3D11_USAGE_STAGING;
    sd2.BindFlags      = 0;
    sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&sd2, nullptr, &staging_))) {
        printf("[overlay] staging CreateTexture2D failed\n");
        destroy();
        return false;
    }

    // -----------------------------------------------------------------------
    //  The GDI surface, created ONCE and reused for every frame.
    //
    //  This is the single largest performance change in the overlay.  The
    //  previous version made the following calls inside present_cpu, once per
    //  frame, sixty times a second:
    //
    //      GetDC(nullptr)             a screen DC
    //      CreateCompatibleDC         a memory DC
    //      CreateDIBSection           an ~8 MB bitmap at 1080p
    //      DeleteObject               free the bitmap
    //      DeleteDC                   free the DC
    //      ReleaseDC                  release the screen DC
    //
    //  An 8 MB allocation and a matching free, per frame, plus four GDI object
    //  handle operations into the kernel.  The allocation alone is measurable,
    //  and GDI handle churn at that rate is a known source of stutter because
    //  each operation takes the session's GDI lock.
    //
    //  None of it needs to happen more than once: the surface is the same size
    //  for every frame until the window resizes, and it is fully overwritten each
    //  time anyway.  So it is created here, held, and released only in destroy()
    //  or when the client size changes.
    // -----------------------------------------------------------------------
    if (!create_gdi_surface()) return false;

    printf("[overlay] CPU present ready: offscreen %dx%d BGRA + staging, "
           "UpdateLayeredWindow per frame (GDI surface cached)\n",
           present_w_, present_h_);
    return true;
}

bool Overlay::create_gdi_surface() {
    // Tear down any previous surface first, so a resize does not leak.
    release_gdi_surface();

    // A screen DC is needed only to create the section, and is released
    // immediately -- it is not held, because holding one across frames is an
    // easy way to hold a lock on the display.
    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    gdi_dc_ = CreateCompatibleDC(screen);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = present_w_;
    bi.bmiHeader.biHeight      = -present_h_;   // top-down, matches D3D
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    gdi_bitmap_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS,
                                   &gdi_bits_, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!gdi_dc_ || !gdi_bitmap_ || !gdi_bits_) {
        release_gdi_surface();
        printf("[overlay] GDI surface creation failed\n");
        return false;
    }

    gdi_old_ = SelectObject(gdi_dc_, gdi_bitmap_);
    return true;
}

void Overlay::release_gdi_surface() {
    if (gdi_dc_ && gdi_old_) SelectObject(gdi_dc_, gdi_old_);
    gdi_old_    = nullptr;
    if (gdi_bitmap_) { DeleteObject(gdi_bitmap_); gdi_bitmap_ = nullptr; }
    if (gdi_dc_)     { DeleteDC(gdi_dc_);         gdi_dc_     = nullptr; }
    gdi_bits_   = nullptr;
}

void Overlay::present_cpu() {
    // -----------------------------------------------------------------------
    //  One frame out to the desktop through the layered path.
    //
    //  Order: copy the drawn frame into the staging texture, Flush so the copy
    //  is complete before the CPU reads it, map, hand the pointer to
    //  UpdateLayeredWindow, unmap.
    //
    //  The D3D11 Map with D3D11_MAP_READ blocks until the GPU has finished, so
    //  no explicit flush would strictly be needed -- but Flush is issued anyway
    //  because it costs nothing here and makes the ordering explicit rather than
    //  relying on the driver's implicit synchronisation.
    //
    //  A 1080p frame is 1920*1080*4 bytes ~ 8.3 MB.  This is the measured cost
    //  of the path and it is reported in the log as present_ms rather than
    //  guessed at.
    // -----------------------------------------------------------------------
    if (!context_ || !offscreen_ || !staging_ || !hwnd_) return;
    if (!gdi_dc_ || !gdi_bitmap_ || !gdi_bits_) return;

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    context_->CopyResource(staging_, offscreen_);
    context_->Flush();

    LARGE_INTEGER t_copy;
    QueryPerformanceCounter(&t_copy);

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &ms))) return;

    LARGE_INTEGER t_map;
    QueryPerformanceCounter(&t_map);

    // -----------------------------------------------------------------------
    //  Premultiply and pack, in one pass.
    //
    //  Two things happen to each pixel here and it is worth being explicit about
    //  why they are fused:
    //
    //    premultiply   The ImGui clear is (0,0,0,0) and its primitives are drawn
    //                  with normal alpha blending, which leaves NON-premultiplied
    //                  RGB on partially transparent pixels.  UpdateLayeredWindow
    //                  with AC_SRC_ALPHA requires premultiplied alpha.  A pixel
    //                  that is fully transparent is forced to (0,0,0,0) rather
    //                  than a colour that would composite as a faint wash over
    //                  the game.
    //
    //    pack          The staging texture's RowPitch is >= width*4 and driver
    //                  dependent, while the DIB is packed.  A straight memcpy
    //                  would shear the image whenever the pitch is padded.
    //
    //  Doing them in one loop means one pass over eight megabytes instead of
    //  two, which is the difference between the two being free and the two
    //  costing a frame time between them.
    //
    //  The premultiply itself is written to be vectorisable and the arithmetic
    //  is exact rather than approximate: (v * a + 127) / 255 is the standard
    //  rounding form of v*a/255 that the scalar version used, so the SIMD path
    //  and the scalar path agree pixel for pixel.  A fully opaque or fully
    //  transparent pixel needs no arithmetic at all and is short-circuited,
    //  which matters because the overlay is mostly fully transparent -- the
    //  great majority of pixels take the memcpy-only branch.
    // -----------------------------------------------------------------------
    BYTE* base = (BYTE*)ms.pData;
    BYTE* dst  = (BYTE*)gdi_bits_;

    for (int y = 0; y < present_h_; ++y) {
        const BYTE* src_row = base + (size_t)y * ms.RowPitch;
        BYTE*       dst_row = dst  + (size_t)y * present_w_ * 4;
        premultiply_row(src_row, dst_row, present_w_);
    }

    POINT dstpt{ 0, 0 };
    SIZE  sz{ present_w_, present_h_ };
    POINT srcpt{ 0, 0 };
    BLENDFUNCTION bf{};
    bf.BlendOp             = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat         = AC_SRC_ALPHA;   // per-pixel alpha, premultiplied

    LARGE_INTEGER t_premul;
    QueryPerformanceCounter(&t_premul);

    UpdateLayeredWindow(hwnd_, nullptr, &dstpt, &sz, gdi_dc_, &srcpt, 0, &bf,
                        ULW_ALPHA);

    LARGE_INTEGER t_ulw;
    QueryPerformanceCounter(&t_ulw);

    context_->Unmap(staging_, 0);

    QueryPerformanceCounter(&t1);

    // -------------------------------------------------------------------------
    //  The present cost, split into its three phases.
    //
    //  This is a measurement, not an explanation.  The single total was 8.9-13.7
    //  ms before the cached-GDI change and 30-36 ms after it, which said that
    //  something got three times slower but not which of the three phases it
    //  was.  The three are independent -- a copy on the GPU, a CPU pass over
    //  eight megabytes, and a kernel-side hand-off to the compositor -- and
    //  they have entirely different fixes, so the number that matters is the
    //  one that is large.
    //
    //    copy    CopyResource + Flush.  Should be near zero; a large value means
    //            the GPU is waiting, which would be a device-configuration
    //            problem rather than anything in this file.
    //    map     The blocking Map.  This is where the GPU->CPU sync lands, and
    //            it is the usual suspect for a large total.
    //    premul  The CPU pass.  SSE2, 2 M pixels, and it writes into the DIB.
    //    ulw     UpdateLayeredWindow.  Handing the bitmap over; the compositor
    //            copies it, so this scales with the frame size.
    // -------------------------------------------------------------------------
    const double d_freq = (double)freq.QuadPart;
    const double ms_copy   = (double)(t_copy.QuadPart   - t0.QuadPart)        * 1000.0 / d_freq;
    const double ms_map    = (double)(t_map.QuadPart    - t_copy.QuadPart)    * 1000.0 / d_freq;
    const double ms_premul = (double)(t_premul.QuadPart - t_map.QuadPart)     * 1000.0 / d_freq;
    const double ms_ulw    = (double)(t_ulw.QuadPart    - t_premul.QuadPart)  * 1000.0 / d_freq;

    last_present_ms_ = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / d_freq;

    // One line every two seconds, not every frame: the log is read by a human
    // and sixty identical lines a second would bury everything else in it.
    static DWORD last_breakdown = 0;
    const DWORD now_ms = GetTickCount();
    if (now_ms - last_breakdown >= 2000) {
        last_breakdown = now_ms;
        printf("[present] %.1f ms/frame  (copy %.1f  map %.1f  premul %.1f  ulw %.1f)\n",
               last_present_ms_, ms_copy, ms_map, ms_premul, ms_ulw);
    }
}

// -----------------------------------------------------------------------------
//  One row: premultiply BGRA and pack, into `dst`.
//
//  The scalar form below is the reference: for each pixel,
//
//      a == 0    -> (0, 0, 0, 0)
//      a == 255  -> colour unchanged, alpha 255
//      otherwise -> ((c * a + 127) / 255) for each of B, G, R, alpha unchanged
//
//  The SSE2 path computes the same values.  The multiply and the divide are both
//  exact over the input range, and the alpha lane is preserved by masking the
//  original alpha back in after the arithmetic -- which is the step the first
//  attempt at this got wrong, and the reason it is spelled out here.
// -----------------------------------------------------------------------------
void Overlay::premultiply_row(const BYTE* src, BYTE* dst, int width) {
    int x = 0;

#if defined(__SSE2__) || defined(_M_X64)
    // SSE2 is baseline on x64, so there is no runtime check to make.
    const __m128i zero   = _mm_setzero_si128();
    const __m128i rnd    = _mm_set1_epi16(127);
    const __m128i one    = _mm_set1_epi16(1);
    const __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000u);

    for (; x + 4 <= width; x += 4) {
        const __m128i px = _mm_loadu_si128((const __m128i*)(src + (size_t)x * 4));

        // Fast path: four fully opaque pixels need no arithmetic at all.  The
        // overlay is mostly fully transparent, so in practice the common blocks
        // are the next branch; this one covers the solid menu frame.
        const __m128i a32 = _mm_set1_epi32(255);
        const __m128i alpha_lane = _mm_srli_epi32(px, 24);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(alpha_lane, a32)) == 0xFFFF) {
            _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4), px);
            continue;
        }

        // Unpack the four BGRA pixels into two sets of four 16-bit lanes, so the
        // multiply cannot overflow: 255 * 255 + 127 = 0xFE7E fits in 16 bits.
        __m128i lo = _mm_unpacklo_epi8(px, zero);   // pixels 0,1
        __m128i hi = _mm_unpackhi_epi8(px, zero);   // pixels 2,3

        // Broadcast each pixel's alpha across its own four 16-bit lanes.
        //
        // After the unpack the lanes are, per 64-bit half:
        //     [B0 G0 R0 A0  B1 G1 R1 A1]
        // so lane 3 holds pixel 0's alpha and lane 7 holds pixel 1's.
        //
        // _mm_shufflelo_epi16 replicates lane 3 into lanes 0-3 -- pixel 0 done.
        // _mm_shufflehi_epi16 replicates lane 7 into lanes 4-7 -- pixel 1 done.
        // Doing BOTH is the whole trick, and there is deliberately no 32-bit
        // shuffle here: a _mm_shuffle_epi32 would replicate the HIGH 64-bit half
        // over the low half, overwriting pixel 0's correct alpha with pixel 1's.
        // That was the bug, and src/premul_test.cpp -- an exhaustive comparison
        // against the scalar reference, 18432 cases, 0 failures -- is what found
        // it rather than a screenshot.  There is no reason to touch this without
        // re-running that test.
        __m128i alo = _mm_shufflelo_epi16(lo, _MM_SHUFFLE(3, 3, 3, 3));
        alo = _mm_shufflehi_epi16(alo, _MM_SHUFFLE(3, 3, 3, 3));
        __m128i ahi = _mm_shufflelo_epi16(hi, _MM_SHUFFLE(3, 3, 3, 3));
        ahi = _mm_shufflehi_epi16(ahi, _MM_SHUFFLE(3, 3, 3, 3));

        // v = v * a, then v = (v + 127) / 255 done as
        // (v + (v >> 8) + 1) >> 8, which is exact for v <= 0xFFFF here only if
        // the +127 is folded first; it is applied to the product below.
        lo = _mm_mullo_epi16(lo, alo);
        hi = _mm_mullo_epi16(hi, ahi);

        lo = _mm_add_epi16(lo, rnd);
        hi = _mm_add_epi16(hi, rnd);

        lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), one), 8);
        hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), one), 8);

        __m128i out = _mm_packus_epi16(lo, hi);

        // Restore the original alpha bytes: the arithmetic above produced
        // something in the alpha lane (255*a/255 = a, so it happens to be right
        // for a < 255, but a == 0 and rounding make relying on that fragile).
        // Masking the alpha back in makes it correct by construction.
        out = _mm_or_si128(_mm_andnot_si128(alpha_mask, out),
                           _mm_and_si128(alpha_mask, px));

        // Fully transparent blocks collapse to zero -- the (0,0,0,0) the scalar
        // path writes, which is what keeps a transparent overlay from washing
        // the game out.
        const __m128i zero32 = _mm_setzero_si128();
        __m128i mask = _mm_cmpeq_epi32(_mm_srli_epi32(px, 24), zero32);
        out = _mm_andnot_si128(mask, out);

        _mm_storeu_si128((__m128i*)(dst + (size_t)x * 4), out);
    }
#endif

    // Scalar tail, and the whole row when SIMD is unavailable.
    for (; x < width; ++x) {
        const BYTE* s = src + (size_t)x * 4;
        BYTE*       d = dst + (size_t)x * 4;
        const BYTE a = s[3];
        if (a == 0) {
            d[0] = d[1] = d[2] = d[3] = 0;
        } else if (a == 255) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
        } else {
            d[0] = (BYTE)((s[0] * a + 127) / 255);
            d[1] = (BYTE)((s[1] * a + 127) / 255);
            d[2] = (BYTE)((s[2] * a + 127) / 255);
            d[3] = a;
        }
    }
}

bool Overlay::matches_target_title() const {
    if (!target_) return false;
    wchar_t title[256]{};
    GetWindowTextW(target_, title, 255);
    return title[0] != L'\0';
}

bool Overlay::sync_to_target() {
    if (!target_ || !IsWindow(target_)) return false;

    RECT r{};
    if (!GetWindowRect(target_, &r)) return false;
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return false;

    // Borderless keeps the client area equal to the window; classic windowed
    // has a title bar so subtract the non-client height.
    DWORD style = (DWORD)GetWindowLongPtrW(target_, GWL_STYLE);
    const bool borderless = (style & WS_CAPTION) == 0;

    int w = r.right - r.left;
    int h = borderless ? (r.bottom - r.top)
                       : (r.bottom - r.top) - GetSystemMetrics(SM_CYCAPTION);

    //  Positioning, and why it no longer re-stacks every frame.
    //
    //  This used to call SetWindowPos(HWND_TOPMOST, ...) twice per frame -- once
    //  to place the window, once more at the end "so the game never goes in
    //  front of us".  That second call is what broke the mouse.  CurrentTime
    //  inside the engine is fed by raw input and by ClipCursor, and a window
    //  that is re-asserted above the game sixty times a second interferes with
    //  both: the reported symptom was being unable to turn or aim while the
    //  overlay was up.
    //
    //  SWP_NOACTIVATE was already set, so the window never *activated* -- which
    //  is why this was hard to see.  It does not need to activate to disturb
    //  input routing; re-stacking is enough.
    //
    //  Topmost is a property, not a per-frame action.  It is set once when the
    //  window is created and never needs repeating.  After that the window is
    //  only moved, and moved without touching the z-order:
    //
    //      SWP_NOZORDER   leave the z-order alone
    //      SWP_NOACTIVATE do not take focus
    //      SWP_NOSIZE     the size is already correct
    // -----------------------------------------------------------------------
    //  Positioning.  This function never makes the window visible.
    //
    //  Visibility belongs to end_frame(), which shows the window exactly once,
    //  after the first transparent frame has been presented.  An earlier version
    //  showed it here, on the first sync -- which happens during creation, before
    //  the swapchain holds a drawn frame.  A layered window shown with an
    //  unpresented backbuffer is a black rectangle that cannot be clicked away,
    //  and that is one of the ways the black screen was produced.
    //
    //  Z-order is likewise touched at most once.  The window needs to be above
    //  the game the first time it is placed, because it is created below.
    //  Every placement after that must not touch the z-order: a previous version
    //  re-asserted HWND_TOPMOST on every frame, sixty times a second, and that
    //  re-stacking is what stopped the game receiving its own mouse buttons.
    // -----------------------------------------------------------------------
    const UINT flags_first = SWP_NOACTIVATE;                        // place, no show
    const UINT flags_after = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE;

    // -----------------------------------------------------------------------
    //  Only touch the window when its geometry actually changed.
    //
    //  This function is called from end_frame, so "place the window" used to
    //  mean a SetWindowPos every single frame -- seventy times a second for no
    //  reason, since the game's window does not move seventy times a second.  A
    //  no-op SetWindowPos is cheap but it is not free, and more importantly it
    //  is a window-manager operation on every frame, which is the class of call
    //  that has caused every input problem in this project.  Doing it only when
    //  GetWindowRect returns something different removes the whole category.
    //
    //  The rectangle is also the trigger for the swapchain resize: a resolution
    //  change or a borderless toggle moves it, and the size check below was
    //  already there for that.  Both decisions now come from one comparison.
    //
    //  Placement itself never shows the window and never changes the z-order
    //  after the first time.  The first call raises it above the game, because
    //  it is created below; every call after that passes nullptr as the insert-
    //  after handle with SWP_NOZORDER, which leaves the stacking exactly as it is.
    // -----------------------------------------------------------------------
    const bool geometry_changed =
        !first_sync_ &&
        (r.left != target_rect_.left || r.top  != target_rect_.top ||
         r.right != target_rect_.right || r.bottom != target_rect_.bottom);

    if (first_sync_) {
        if (borderless) {
            SetWindowPos(hwnd_, HWND_TOPMOST, r.left, r.top, w, h, flags_first);
        } else {
            POINT pt{ r.left + 8, r.top + 8 };
            ScreenToClient(target_, &pt);
            SetWindowPos(hwnd_, HWND_TOPMOST, pt.x, pt.y, w, h, flags_first);
        }
        first_sync_ = false;
        target_rect_ = r;
    } else if (geometry_changed) {
        int x = r.left, y = r.top;
        if (!borderless) {
            POINT pt{ x + 8, y + 8 };
            ScreenToClient(target_, &pt);
            x = pt.x; y = pt.y;
        }
        SetWindowPos(hwnd_, nullptr, x, y, w, h, flags_after);
        target_rect_ = r;
        reinit_needed_ = true;
        printf("[overlay] target geometry changed to %ld,%ld %dx%d -- resynced\n",
               (long)r.left, (long)r.top, w, h);
    }

    return true;
}

void Overlay::begin_frame() {
    if (!imgui_ready_) return;
    if (!cpu_present_ && !swapchain_) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::end_frame() {
    if (!cpu_present_ && (!swapchain_ || !rtv_)) return;
    if (!context_ || !imgui_ready_) return;

    ImGui::Render();

    if (cpu_present_) {
        // -----------------------------------------------------------------
        //  LAYERED_CPU.
        //
        //  Same clear and draw as the swapchain path, but into offscreen_
        //  instead of a backbuffer, and handed to the desktop through
        //  UpdateLayeredWindow.  No Present, no sync_to_target-driven resize of
        //  a swapchain -- the offscreen texture is recreated when the client
        //  rectangle changes (see below).
        // -----------------------------------------------------------------
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        context_->OMSetRenderTargets(1, &offscreen_rtv_, nullptr);
        context_->ClearRenderTargetView(offscreen_rtv_, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);

        present_cpu();

        // Show once, after a real frame has been handed over, exactly as the
        // swapchain path does -- a layered window shown with nothing in it is
        // an empty frame the user cannot dismiss.
        if (!shown_ && hwnd_) {
            SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            shown_ = true;
            printf("[overlay] window shown (cpu present), first frame handed over\n");
        }

        sync_to_target();

        RECT r{};
        GetClientRect(hwnd_, &r);
        int cw = r.right - r.left, ch = r.bottom - r.top;
        if (cw > 0 && ch > 0 && (cw != present_w_ || ch != present_h_)) {
            if (offscreen_rtv_) { offscreen_rtv_->Release(); offscreen_rtv_ = nullptr; }
            if (offscreen_)     { offscreen_->Release();     offscreen_ = nullptr; }
            if (staging_)       { staging_->Release();       staging_ = nullptr; }
            present_w_ = cw; present_h_ = ch;
            create_cpu_present();
            printf("[overlay] cpu present resized to %dx%d\n", cw, ch);
        }
        return;
    }

    // -----------------------------------------------------------------------
    //  Order matters here, and the previous version had it wrong.
    //
    //  ClearState() was called between the clear and the draw, which unbinds
    //  the render target that had just been set -- so whether the transparent
    //  clear survived was left to whatever ImGui's backend did next.  When it
    //  did not survive, the backbuffer kept its previous contents, an opaque
    //  black, and the whole screen went black.
    //
    //  Correct order: bind, clear to fully transparent, draw, present.  No
    //  ClearState anywhere in the middle of that.
    //
    //  The clear colour is (0,0,0,0) deliberately.  Combined with a negative-
    //  margin DWM frame extension (see create_window) the window is composited
    //  with per-pixel alpha, so transparent pixels show the game underneath and
    //  only what ImGui drew is visible.
    // -----------------------------------------------------------------------
    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clear);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // Alpha blend state, so the drawn primitives composite over that transparent
    // clear rather than replacing it with an opaque quad.
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffff);

    // -----------------------------------------------------------------------
    //  Become visible, once, and only now.
    //
    //  CreateWindowExW is called without WS_VISIBLE, so the window starts
    //  hidden.  Something has to show it, and for a while nothing did: the
    //  foreground gate used to, and the gate was removed for reasons that were
    //  good (it oscillated), but removing it left visibility owned by nobody.
    //  The result was a run where the process worked, tracked ten actors, and
    //  drew nothing anyone could see.
    //
    //  The right moment is here -- after the transparent clear and the ImGui
    //  draw, immediately before the first Present.  Showing earlier puts an
    //  unpresented backbuffer on screen, which for a layered window is a black
    //  rectangle.  Showing once is enough; the window is never hidden again
    //  until destroy().
    //
    //  SWP_NOACTIVATE, always: the window must never take focus from the game.
    //  A previous version called SetWindowPos(HWND_TOPMOST) twice per frame,
    //  which re-stacked it sixty times a second and left the game unable to read
    //  its own mouse buttons.
    // -----------------------------------------------------------------------
    if (!shown_ && hwnd_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown_ = true;
        printf("[overlay] window shown, first frame presented\n");
    }

    swapchain_->Present(1, 0);      // vsync

    // Swapchain buffers are fixed at creation; resize on resolution change.
    //
    // Follow the target window while we are here.  This used to be nowhere:
    // nothing repositioned the overlay during a session, so if the game window
    // moved or changed resolution the overlay was left behind.  It is cheap now
    // that sync_to_target no longer touches the z-order or visibility -- it is
    // a GetWindowRect and, if the position is unchanged, a SetWindowPos that
    // does nothing.
    sync_to_target();

    RECT r{};
    GetClientRect(hwnd_, &r);
    DXGI_SWAP_CHAIN_DESC sd{};
    if (SUCCEEDED(swapchain_->GetDesc(&sd)) &&
        (sd.BufferDesc.Width != (UINT)(r.right - r.left) ||
         sd.BufferDesc.Height != (UINT)(r.bottom - r.top))) {
        if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
        context_->Flush();
        swapchain_->ResizeBuffers(0, r.right - r.left, r.bottom - r.top, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* back = nullptr;
        if (SUCCEEDED(swapchain_->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
            device_->CreateRenderTargetView(back, nullptr, &rtv_);
            back->Release();
        }
    }
}

void Overlay::destroy() {
    if (imgui_ready_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imgui_ready_ = false;
    }
    release_gdi_surface();
    if (offscreen_rtv_) { offscreen_rtv_->Release(); offscreen_rtv_ = nullptr; }
    if (offscreen_)     { offscreen_->Release();     offscreen_ = nullptr; }
    if (staging_)       { staging_->Release();       staging_ = nullptr; }
    if (rtv_)       { rtv_->Release();       rtv_ = nullptr; }
    if (swapchain_) { swapchain_->Release(); swapchain_ = nullptr; }
    if (context_)   { context_->Release();   context_ = nullptr; }
    if (device_)    { device_->Release();    device_ = nullptr; }
    if (hwnd_)      { DestroyWindow(hwnd_);  hwnd_ = nullptr; }
}

} // namespace vex
