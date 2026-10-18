// -----------------------------------------------------------------------------
//  menu.hpp  --  the menu and the visual state it edits.
//
//  There is ONE configuration state in this program, and it is UiState below.
//  The menu writes to it and the ESP renderer reads from it, so a control in the
//  menu and the box on the game screen can never disagree -- a screenshot that
//  does not match the switch would otherwise be the first question asked of any
//  bug report, and answering it costs a run every time.
//
//  What is deliberately absent:
//
//    * No connection status, no invented FPS, no "injected" badge.  The spec
//      forbids fake operational state and it is right to: this menu has one
//      working path and no session concept, so any such readout would be
//      decoration pretending to be telemetry.
//
//    * No name or health controls wired to real data.  The read path has no
//      measured offset for either.  They are shown, disabled, and labelled, per
//      the spec -- shown rather than hidden so the layout is real and the gap is
//      visible, and disabled rather than faked so nothing draws a number that
//      was never read.
//
//  The state is split in two: EspVisual holds what the renderer consumes, and
//  AimAssist holds a structure that currently drives a simulation only.  The
//  split is not tidiness -- it is the boundary between "this draws on the game"
//  and "this does not yet touch anything", and keeping it explicit is what makes
//  the second one safe to have in the binary at all.
// -----------------------------------------------------------------------------
#pragma once

#include <Windows.h>
#include <imgui.h>
#include <string>

namespace vex {
namespace bs {

/// Colours, in one place, as the spec gives them.  Named rather than inlined so
/// the palette can be compared against the document line by line.
namespace theme {
    constexpr ImU32 kMenuBg       = IM_COL32(0x14, 0x14, 0x16, 0xFF);  // #141416
    constexpr ImU32 kSidebarBg    = IM_COL32(0x10, 0x10, 0x12, 0xFF);  // #101012
    constexpr ImU32 kCardBg       = IM_COL32(0x1B, 0x1B, 0x1E, 0xFF);  // #1B1B1E
    constexpr ImU32 kTabActiveBg  = IM_COL32(0x2A, 0x2A, 0x2E, 0xFF);  // #2A2A2E
    constexpr ImU32 kOuterBorder  = IM_COL32(0x30, 0x30, 0x31, 0xFF);  // #303031
    constexpr ImU32 kDivider      = IM_COL32(0x29, 0x29, 0x2B, 0xFF);  // #29292B
    constexpr ImU32 kCardBorder   = IM_COL32(0x2C, 0x2C, 0x2F, 0xFF);  // #2C2C2F
    constexpr ImU32 kTextPrimary  = IM_COL32(0xE8, 0xE8, 0xE8, 0xFF);  // #E8E8E8
    constexpr ImU32 kTextSecond   = IM_COL32(0x92, 0x92, 0x97, 0xFF);  // #929297
    constexpr ImU32 kSectionTitle = IM_COL32(0xA8, 0xA8, 0xAD, 0xFF);  // #A8A8AD
    constexpr ImU32 kAccent       = IM_COL32(0xB5, 0xB5, 0xBA, 0xFF);  // #B5B5BA

    constexpr ImU32 kSwitchOn     = IM_COL32(0xB5, 0xB5, 0xBA, 0xFF);
    constexpr ImU32 kSwitchKnobOn = IM_COL32(0x18, 0x18, 0x19, 0xFF);
    constexpr ImU32 kSwitchOff    = IM_COL32(0x39, 0x39, 0x3E, 0xFF);
    constexpr ImU32 kSwitchKnob   = IM_COL32(0x93, 0x93, 0x99, 0xFF);

    constexpr ImU32 kPreviewBg    = IM_COL32(0x0C, 0x0C, 0x0D, 0xFF);
    constexpr ImU32 kPreviewGlow  = IM_COL32(0x26, 0x26, 0x29, 0x22);
    constexpr ImU32 kPreviewBorder= IM_COL32(0x29, 0x29, 0x2D, 0xFF);
    constexpr ImU32 kPreviewBody  = IM_COL32(0x50, 0x50, 0x57, 0xFF);  // #505057

    constexpr ImU32 kButtonBg     = IM_COL32(0x24, 0x24, 0x26, 0xFF);
    constexpr ImU32 kButtonBorder = IM_COL32(0x37, 0x37, 0x3B, 0xFF);
    constexpr ImU32 kButtonHover  = IM_COL32(0x30, 0x30, 0x34, 0xFF);

    constexpr ImU32 kHealthBar    = IM_COL32(0xA7, 0xB7, 0xA7, 0xFF);  // #A7B7A7

    /// Default element colour, #C2C2C2, as floats for ImGui's color edit.
    constexpr float kElementColor[3] = { 0xC2 / 255.f, 0xC2 / 255.f, 0xC2 / 255.f };
}

/// Everything the ESP renderer reads.  The menu edits exactly these fields and
/// nothing else, which is what keeps the two in step.
struct EspVisual {
    bool  enabled       = true;   ///< master switch
    bool  boxes         = true;
    bool  skeleton      = false;  ///< still supported, off by default per spec §8
    bool  radar         = true;
    bool  show_names    = false;  ///< PENDING: no measured name offset
    bool  show_health   = false;  ///< PENDING: no measured health offset
    bool  show_distance = true;

    /// Element colour, linear 0..1 RGB.  Applied to box, name and distance text.
    float color[3] = { theme::kElementColor[0], theme::kElementColor[1], theme::kElementColor[2] };

    /// Draw distance, metres.  Spec §6 gives 10..500 step 10 default 200.
    ///
    /// The project's own pre-existing range was 20..1500, chosen when the pool
    /// still admitted distant scenery and the useful cut was high.  The spec's
    /// band is what a player actually uses, and 200 m is well inside the range
    /// where the read path is reliable, so the spec wins and the difference is
    /// noted here rather than silently kept.
    float max_distance = 200.f;

    bool  show_fps    = true;
    float box_height  = 1.80f;    ///< world height fallback, unchanged
};

/// Aim assist configuration.  THIS DOES NOT MOVE THE MOUSE.
///
/// It exists so the second tab has a real structure to bind to, and so the
/// selection and smoothing maths can be validated against simulated targets
/// before any of it is connected to input.  See the spec's own ordering: the
/// menu first, the assist as a separate later step.  The defaults are therefore
/// "off", and the preview draws from AimAssist::sim, never from a live actor.
struct AimAssist {
    bool  enabled   = false;                 ///< default OFF, and stays off by default
    int   vk_key    = VK_XBUTTON2;           ///< activation key; mouse 5 by default
    bool  key_down  = false;                 ///< filled by the sim only, never by a real hook
    float radius_px = 120.f;                 ///< selection radius on screen
    float smoothing = 6.f;                   ///< higher is slower; 1 = instant
    int   region    = 0;                     ///< 0 = whole screen, 1 = circle, 2 = box

    /// Isolated simulation state.  Drives the preview only.
    struct Sim {
        bool  on = false;
        ImVec2 target{};       ///< simulated target on the preview surface
        ImVec2 cursor{};       ///< simulated virtual cursor
        bool  in_range = false;
        float pull = 0.f;      ///< interpolated 0..1, for the preview to draw
    } sim;
};

/// The two tabs.  No more: the spec says not to add extra tabs, and a third one
/// would need a purpose this stage does not have.
enum class Tab { Esp = 0, AimAssist = 1 };

/// The menu's own state: which tab, what the mouse is doing, whether the preview
/// is on.  Separate from EspVisual because none of it is drawn on the game.
struct MenuState {
    bool  open      = false;
    Tab   tab       = Tab::Esp;
    bool  sim_on    = true;      ///< "Simulação" toggle in the preview header
    bool  reset_requested = false;  ///< set by the footer button, cleared by the menu

    /// Last drawn rect, for hit-testing when the mouse is free.
    ImVec2 pos{ 0, 0 };
    ImVec2 size{ 0, 0 };

    /// Mouse polling state.  The menu feeds ImGui from GetCursorPos rather than
    /// from the window's WM_MOUSEMOVE, because the overlay window is layered and
    /// click-through and must never take the capture -- see feed_mouse().
    bool   mouse_prev_down = false;
    /// Whether the clip has been released for the menu.  Tracks the transition
    /// so ClipCursor(NULL) happens once on open, not once per frame.
    bool   mouse_gate_open = false;
};

/// The whole configuration, as one object.
struct UiState {
    EspVisual esp{};
    AimAssist aim{};
    MenuState menu{};

    /// Restore the visual settings to their defaults.  Only the visual ones:
    /// nothing outside this struct is touched, per spec §9.
    void restore_defaults();
};

/// Draw the menu.  Returns the ImGui frame's work, but owns no resources.
///
/// Called from the overlay frame while the menu is open.  It draws the window,
/// the sidebar, the cards and the preview, edits `st.esp` / `st.aim`, and
/// reports through st.menu whether the user asked to close.
void draw_menu(UiState& st);

/// Feed the mouse into ImGui from the OS directly.
///
/// The overlay runs with ImGuiConfigFlags_NoMouse, deliberately: that flag is
/// what keeps the backend from ever calling ::SetCapture, and a window that takes
/// the capture keeps the mouse until it gives it back -- which is the reported
/// "the pointer appears and the game ignores clicks".  NoMouse must stay.
///
/// But the menu has to be clickable.  So the position comes from GetCursorPos and
/// the buttons from GetAsyncKeyState, and those are handed to ImGui as raw input
/// events.  Nothing is captured, nothing is re-shown, and when the menu closes
/// this function simply stops being called and the game has its mouse untouched.
///
/// `overlay_hwnd` is the window the client coordinates are relative to; it is the
/// overlay, whose rect sits over the game.  Screen-to-client is done by hand with
/// GetWindowRect rather than ScreenToClient, because ScreenToClient on a window
/// this process does not own is not guaranteed to be meaningful and the rect is
/// all that is needed anyway.
///
/// While the menu is open this also releases the game's ClipCursor so the pointer
/// can reach the menu at all.  That release is the single deliberate intrusion
/// into the game's input, it happens once on open and once on close, and it is
/// the reason the caller must call this with the true open/closed state every
/// frame rather than only when it changes.
void feed_mouse(UiState& st, bool menu_open, HWND overlay_hwnd);

/// Load the UI font at a DPI-appropriate size.  Called once, after the ImGui
/// context exists and before the first frame.
void load_font(float dpi_scale);

/// Scale factor from the monitor's DPI (96 = 1.0).  The spec asks for DPI-scaled
/// sizes and this is the single number they all multiply.
float dpi_scale();

/// Human-readable name for a virtual key, for the aim-assist keybind display.
std::string key_name(int vk);

} // namespace bs
} // namespace vex
