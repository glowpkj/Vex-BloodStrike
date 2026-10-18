// -----------------------------------------------------------------------------
//  overlay.hpp  --  click-through transparent window + DX11 swapchain
// -----------------------------------------------------------------------------
#pragma once

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <emmintrin.h>
#include <string>

namespace vex {

class Overlay {
public:
    ~Overlay();

    bool create(const std::wstring& target_title);

    /// Attach to a window handle the caller already has.
    ///
    /// create() locates the game by class name or by title.  Blood Strike's
    /// window has neither a known class nor a title FindWindowW can match, and
    /// it is found reliably by walking the process list instead -- so hand the
    /// handle over rather than round-tripping it through a string that may well
    /// be empty.
    bool create_on(HWND target);

    /// Present path selection, and why there are two.
    ///
    /// A window can show D3D content to the desktop in exactly two ways, and
    /// they are mutually exclusive:
    ///
    ///   SWAPCHAIN (the default)
    ///       DwmExtendFrameIntoClientArea gives per-pixel alpha and the DWM
    ///       composites the swapchain itself.  Drawing is perfect -- every ESP
    ///       box has appeared with this path.  Input is not: the hit-test still
    ///       sees a solid rectangle, so WS_TRANSPARENT does not forward the
    ///       mouse and clicks die on the overlay on every window on the desktop.
    ///
    ///   LAYERED_CPU (set by set_cpu_present, the --ulw switch)
    ///       The window is WS_EX_LAYERED and its content is supplied by
    ///       UpdateLayeredWindow from a CPU buffer.  That is the documented,
    ///       cross-process click-through path -- the mouse works.  The cost is
    ///       one GPU->CPU readback per frame, which is measured rather than
    ///       assumed: see the fps line in the log.
    ///
    /// Both paths draw the same ImGui frame; only presentation differs.
    enum class Present { Swapchain, LayeredCpu };

    /// Choose the CPU (UpdateLayeredWindow) presentation path.  Must be called
    /// before create_on()/create().
    void set_cpu_present(bool on) { cpu_present_ = on; }
    bool cpu_present() const { return cpu_present_; }

    /// Last measured frame cost of the CPU present path, in milliseconds.
    double last_present_ms() const { return last_present_ms_; }

    void destroy();

    /// ImGui frame bracketing.  begin_frame() before drawing, end_frame() after.
    void begin_frame();
    void end_frame();

    HWND hwnd()  const { return hwnd_; }
    HWND target() const { return target_; }
    bool ready()  const { return cpu_present_ ? (offscreen_ != nullptr) : (swapchain_ != nullptr); }

    /// True once the window has been shown for the first time.
    ///
    /// The visibility gate must not show the window before the first frame has
    /// been presented -- showing an unpresented backbuffer on a layered window
    /// is a black rectangle -- so it needs to know whether that has happened yet.
    bool shown()  const { return shown_; }

    /// Re-anchor to the game window (handles alt-tab, resolution changes,
    /// borderless transitions).
    bool sync_to_target();
    bool matches_target_title() const;

private:
    bool create_window();
    bool create_device();
    bool create_cpu_present();
    void present_cpu();

    /// Allocate the cached GDI surface used by the layered present path.
    ///
    /// One DC and one DIB section, created when the client size is first known
    /// and re-created only on a resize.  The previous code created both, plus an
    /// ~8 MB bitmap, inside present_cpu every frame -- an allocation and four
    /// GDI handle operations per frame at 60 Hz.
    bool create_gdi_surface();
    void release_gdi_surface();

    /// Premultiply one BGRA row and pack it into the DIB.
    static void premultiply_row(const BYTE* src, BYTE* dst, int width);

    HWND     hwnd_       = nullptr;
    HWND     target_     = nullptr;
    RECT     target_rect_{};

    ID3D11Device*           device_       = nullptr;
    ID3D11DeviceContext*    context_      = nullptr;
    IDXGISwapChain*         swapchain_    = nullptr;
    ID3D11RenderTargetView* rtv_          = nullptr;

    // ---- CPU present path (--ulw) -------------------------------------------
    //
    //  A swapchain is never created on this path.  Instead the frame is drawn
    //  into offscreen_texture_, resolved into staging_, mapped, and handed to
    //  UpdateLayeredWindow.  Three objects and one buffer, and the buffer is
    //  reused every frame so the allocation happens once.
    bool                    cpu_present_     = false;
    ID3D11Texture2D*        offscreen_       = nullptr;
    ID3D11RenderTargetView* offscreen_rtv_   = nullptr;
    ID3D11Texture2D*        staging_         = nullptr;
    void*                   mapped_          = nullptr;
    size_t                  mapped_row_pitch_= 0;
    double                  last_present_ms_ = 0.0;
    int                     present_w_       = 0;
    int                     present_h_       = 0;

    /// The cached GDI surface for UpdateLayeredWindow.  See create_gdi_surface.
    HDC     gdi_dc_     = nullptr;
    HBITMAP gdi_bitmap_ = nullptr;
    void*   gdi_bits_   = nullptr;
    HGDIOBJ gdi_old_    = nullptr;

    bool    imgui_ready_   = false;
    bool    imgui_frame_   = false;
    bool    reinit_needed_ = false;
    /// True once the window has been made visible, after the first frame.
    ///
    /// The window is created without WS_VISIBLE and shown exactly once, here, so
    /// that it is never on screen as an unpresented black rectangle.
    bool    shown_         = false;
    /// True until the overlay has been placed over the game once.
    ///
    /// The first placement has to raise the window above the game, because it
    /// is created below.  Every placement after that must not touch the z-order
    /// at all -- see the note in sync_to_target for why re-stacking per frame
    /// broke the mouse.
    bool    first_sync_    = true;

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
};

} // namespace vex
