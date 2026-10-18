// -----------------------------------------------------------------------------
//  menu.cpp  --  the menu itself: layout, widgets, preview.
//
//  ImGui is used as a drawing library here rather than as a widget framework.
//  The window chrome, the sidebar, the cards and the switches are all drawn
//  explicitly onto draw lists with the theme colours, because the default ImGui
//  style is a blue-and-grey desktop look and restyling every widget to match a
//  34-colour grey palette ends up being more code than drawing them directly --
//  and it fights the framework on every ImGui upgrade.
//
//  What IS used from ImGui: the input routing (InvisibleButton + IsItemHovered /
//  IsItemActive), because hit-testing, overlap and click-and-drag on a slider
//  are exactly the parts worth not reimplementing, and they are correct.
//
//  The preview draws from a simulated actor, never from a live one.  The spec is
//  explicit about this and it matters: the preview must show the STYLE, and
//  showing a real enemy would make the preview depend on being in a match, which
//  would make it useless precisely when the menu is open and no match is running.
// -----------------------------------------------------------------------------
#include "menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vex {
namespace bs {

using namespace theme;

namespace {

// -----------------------------------------------------------------------------
//  Geometry, in reference pixels.  Everything else multiplies these by the DPI
//  scale, so a 4K monitor gets a legible menu and a 1080p one gets the document's
//  proportions without either being hard-coded twice.
// -----------------------------------------------------------------------------
constexpr float kMenuWidth   = 900.f;
constexpr float kMenuMinH    = 650.f;
constexpr float kSidebarW    = 192.f;
constexpr float kOuterRound  = 14.f;
constexpr float kContentPad  = 22.f;
constexpr float kCardGap     = 16.f;
constexpr float kCardRound   = 10.f;
constexpr float kHeaderH     = 85.f;
constexpr float kTabH        = 44.f;
constexpr float kRowH        = 39.f;
constexpr float kPreviewH    = 275.f;

// -----------------------------------------------------------------------------
//  Small helpers.  Each draws one thing and is named after that thing, which is
//  the only reason the layout code below stays readable at this width.
// -----------------------------------------------------------------------------

ImVec4 col(ImU32 c) {
    return ImGui::ColorConvertU32ToFloat4(c);
}

/// Text with a manual letter-spacing, which ImGui has no native support for.
///
/// The spec asks for ~2 px between letters on the "MENU" label and ~1 px on card
/// titles.  ImGui's AddText cannot do that, so each glyph is placed by hand.  The
/// cost is one AddText per character, which is nothing at these sizes -- and the
/// alternative, a second font or a shader, would be far more machinery for a
/// dozen short strings.
void text_spaced(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text,
                 float spacing, float size) {
    ImFont* font = ImGui::GetFont();
    const float fs = (size > 0.f) ? size : ImGui::GetFontSize();
    const float base = ImGui::GetFontSize();
    float x = pos.x;
    for (const char* p = text; *p; ++p) {
        char ch[2] = { *p, '\0' };
        // The font is loaded at base size; scaling here is what makes a single
        // loaded font serve both the 10 px labels and the 16 px title.
        dl->AddText(font, fs, ImVec2(x, pos.y), color, ch);
        x += font->CalcTextSizeA(fs, FLT_MAX, 0.f, ch).x + spacing;
    }
    (void)base;
}

float text_width(const char* text, float size) {
    ImFont* font = ImGui::GetFont();
    return font->CalcTextSizeA(size, FLT_MAX, 0.f, text).x;
}

/// A card: rounded background, 1 px border, and the section title above the
/// content area.  Returns the top of the content area, and takes the content
/// height so it can draw the frame first.
void card_begin(ImDrawList* dl, ImVec2 tl, ImVec2 br, const char* title,
                float scale) {
    dl->AddRectFilled(tl, br, kCardBg, kCardRound * scale);
    dl->AddRect(tl, br, kCardBorder, kCardRound * scale, 0, 1.f * scale);

    text_spaced(dl, ImVec2(tl.x + 16.f * scale, tl.y + 14.f * scale),
                kSectionTitle, title, 1.f * scale, 10.f * scale);
}

/// The switch, per spec §5: 29x16 track, round, 10 px knob.
///
/// Enabled and disabled states are distinct in a way that survives a screenshot:
/// the on state is light with a dark knob, the off state dark with a light knob,
/// so the control is readable even in a greyscale copy of the screen.
///
/// Returns true if it was toggled this frame.
bool toggle_switch(const char* id, bool* value, float scale, bool enabled = true) {
    const float w = 34.f * scale;
    const float h = 19.f * scale;
    const float knob = 13.f * scale;
    const float pad = (h - knob) * 0.5f;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered() && enabled;
    const bool clicked = ImGui::IsItemClicked() && enabled;

    if (clicked) *value = !*value;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 track = *value ? kSwitchOn : kSwitchOff;
    ImU32 knobc = *value ? kSwitchKnobOn : kSwitchKnob;
    if (!enabled) { track = kSwitchOff; knobc = kSwitchKnob; }
    // A hover is a one-step lightening, which is the whole of the feedback: no
    // glow, no animation curve, per the spec's "no exaggerated shadows".
    if (hovered) track = kAccent;

    const float r = h * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, r);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID animation_id = ImGui::GetID(id);
    float t = storage->GetFloat(animation_id, *value ? 1.f : 0.f);
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.f, 0.1f);
    t += ((*value ? 1.f : 0.f) - t) * (1.f - std::exp(-18.f * dt));
    storage->SetFloat(animation_id, t);
    const float kx = p.x + pad + (w - knob - 2.f * pad) * t;
    dl->AddCircleFilled(ImVec2(kx + knob * 0.5f, p.y + h * 0.5f),
                        knob * 0.5f, knobc);
    return clicked;
}

/// One "label on the left, switch on the right" row.
void switch_row(const char* id, const char* label, bool* value, float scale,
                bool enabled = true, float right_edge = 0.f) {
    const float row_h = kRowH * scale;
    ImVec2 p = ImGui::GetCursorScreenPos();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImGui::GetFont(), 12.f * scale,
                ImVec2(p.x, p.y + (row_h - 12.f * scale) * 0.5f),
                enabled ? kTextPrimary : kTextSecond, label);

    // Place the switch at the right edge of the content column.
    const float avail = right_edge > p.x ? right_edge - p.x : ImGui::GetContentRegionAvail().x;
    if (ImGui::IsMouseHoveringRect(p, ImVec2(p.x + avail, p.y + row_h)) && enabled)
        dl->AddRectFilled(p, ImVec2(p.x + avail, p.y + row_h), IM_COL32(255,255,255,5), 5.f * scale);
    ImGui::SetCursorScreenPos(
        ImVec2(p.x + avail - 34.f * scale, p.y + (row_h - 19.f * scale) * 0.5f));
    toggle_switch(id, value, scale, enabled);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
}

/// A row with a label and an arbitrary value string on the right, no interaction.
void value_row(const char* label, const char* value, float scale, float right_edge = 0.f) {
    const float row_h = 30.f * scale;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddText(ImGui::GetFont(), 12.f * scale, p, kTextPrimary, label);

    const float vw = text_width(value, 12.f * scale);
    const float avail = right_edge > p.x ? right_edge - p.x : ImGui::GetContentRegionAvail().x;
    dl->AddText(ImGui::GetFont(), 12.f * scale,
                ImVec2(p.x + avail - vw, p.y), kTextSecond, value);

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h));
}

/// A thin grey slider, per spec §6: "fino, discreto e cinza, sem destaque".
///
/// ImGui's own SliderFloat is themed here rather than reimplemented -- drag,
/// keyboard stepping and click-to-position are all correct in the framework and
/// there is no reason to duplicate them.  Only the colours are overridden, and
/// they are overridden to the palette.
bool thin_slider(const char* id, float* value, float lo, float hi, float step,
                 const char* fmt, float scale) {
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 2.f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 8.f * scale);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        col(ImU32(IM_COL32(0x24, 0x24, 0x26, 0xFF))));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col(IM_COL32(0x2C, 0x2C, 0x2F, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  col(IM_COL32(0x30, 0x30, 0x31, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,     col(IM_COL32(0xB5, 0xB5, 0xBA, 0xFF)));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, col(IM_COL32(0xC8, 0xC8, 0xCC, 0xFF)));
    const bool changed = ImGui::SliderFloat(id, value, lo, hi, fmt,
                                            ImGuiSliderFlags_AlwaysClamp);
    if (changed && step > 0.f)
        *value = std::clamp(lo + std::round((*value - lo) / step) * step, lo, hi);

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    return changed;
}

/// A colour swatch that opens ImGui's picker.  Compact, per spec §6.
bool color_swatch(const char* id, float rgb[3], float scale) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float size = 20.f * scale;

    ImGui::InvisibleButton(id, ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c = IM_COL32((int)(rgb[0] * 255.f), (int)(rgb[1] * 255.f),
                       (int)(rgb[2] * 255.f), 255);
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), c, 3.f * scale);
    dl->AddRect(p, ImVec2(p.x + size, p.y + size),
                hovered ? kAccent : kCardBorder, 3.f * scale, 0, 1.f * scale);

    if (clicked) ImGui::OpenPopup("##colorpicker");
    if (ImGui::BeginPopup("##colorpicker")) {
        ImGui::ColorPicker3("", rgb,
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_NoSmallPreview);
        ImGui::EndPopup();
    }
    return clicked;
}

/// The footer button, per spec §9.
bool footer_button(const char* label, float scale) {
    const float h = 26.f * scale;
    const float w = text_width(label, 11.f * scale) + 24.f * scale;
    ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(label, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      hovered ? kButtonHover : kButtonBg, 4.f * scale);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), kButtonBorder, 4.f * scale, 0,
                1.f * scale);
    dl->AddText(ImGui::GetFont(), 11.f * scale,
                ImVec2(p.x + 12.f * scale, p.y + (h - 11.f * scale) * 0.5f),
                kTextPrimary, label);
    return clicked;
}

// -----------------------------------------------------------------------------
//  The preview.  A fixed illustration: a human silhouette in #505057 with the
//  real ESP styling drawn over it.  Every element follows the same switches the
//  real ESP does, so the preview cannot claim a style the renderer will not
//  produce.
// -----------------------------------------------------------------------------

void draw_preview_silhouette(ImDrawList* dl, ImVec2 tl, ImVec2 br, float scale) {
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    const float cx = (tl.x + br.x) * 0.5f;

    // The silhouette stands where the real box would: feet at 80% of the height,
    // head at 26%.  These are proportions of the surface, so the figure scales
    // with the card instead of being a fixed sprite that clips at some sizes.
    const float foot_y = tl.y + h * 0.80f;
    const float head_y = tl.y + h * 0.26f;
    const float body_h = foot_y - head_y;

    const float head_r = body_h * 0.085f;
    const ImU32 body = kPreviewBody;

    // Head.
    dl->AddCircleFilled(ImVec2(cx, head_y + head_r), head_r, body, 20);

    // Torso: a tapered quad, drawn as two triangles so it reads as shoulders
    // narrowing to hips rather than as a rectangle.
    const float shoulder_y = head_y + head_r * 2.3f;
    const float hip_y      = foot_y - body_h * 0.34f;
    const float shoulder_w = body_h * 0.17f;
    const float hip_w      = body_h * 0.13f;
    dl->AddQuadFilled(ImVec2(cx - shoulder_w, shoulder_y),
                      ImVec2(cx + shoulder_w, shoulder_y),
                      ImVec2(cx + hip_w, hip_y),
                      ImVec2(cx - hip_w, hip_y), body);

    // Arms: two capsules hanging outside the torso.
    const float arm_w = body_h * 0.032f;
    const float arm_top = shoulder_y + body_h * 0.02f;
    const float arm_bot = hip_y + body_h * 0.12f;
    dl->AddRectFilled(ImVec2(cx - shoulder_w - arm_w * 0.2f, arm_top),
                      ImVec2(cx - shoulder_w + arm_w * 0.8f, arm_bot),
                      body, arm_w * 0.5f);
    dl->AddRectFilled(ImVec2(cx + shoulder_w - arm_w * 0.8f, arm_top),
                      ImVec2(cx + shoulder_w + arm_w * 0.2f, arm_bot),
                      body, arm_w * 0.5f);

    // Legs.
    const float leg_w = body_h * 0.045f;
    const float leg_gap = body_h * 0.012f;
    dl->AddRectFilled(ImVec2(cx - leg_gap - leg_w, hip_y),
                      ImVec2(cx - leg_gap, foot_y), body, leg_w * 0.4f);
    dl->AddRectFilled(ImVec2(cx + leg_gap, hip_y),
                      ImVec2(cx + leg_gap + leg_w, foot_y), body, leg_w * 0.4f);
}

void draw_preview_elements(ImDrawList* dl, const EspVisual& v, UiState& st,
                           ImVec2 tl, ImVec2 br, float scale) {
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;
    const float cx = (tl.x + br.x) * 0.5f;

    const ImU32 ec = IM_COL32((int)(v.color[0] * 255.f), (int)(v.color[1] * 255.f),
                              (int)(v.color[2] * 255.f), 255);

    // The box is 90x166 reference px, centred on the figure.
    const float box_w = 90.f * scale;
    const float box_h = 166.f * scale;
    const float box_cx = cx;
    const float box_bottom = tl.y + h * 0.80f + 6.f * scale;
    const float box_top = box_bottom - box_h;

    // Distance gate, per spec §7: below the sample's 42 m the example hides.
    const bool in_range = (v.max_distance >= 42.f);

    if (!v.enabled || !in_range) return;

    if (v.boxes) {
        dl->AddRect(ImVec2(box_cx - box_w * 0.5f, box_top),
                    ImVec2(box_cx + box_w * 0.5f, box_bottom), ec, 0.f, 0,
                    1.f * scale);
    }

    if (v.show_names) {
        const char* name = "Jogador";
        const float tw = text_width(name, 10.f * scale);
        dl->AddText(ImGui::GetFont(), 10.f * scale,
                    ImVec2(box_cx - tw * 0.5f, box_top - 17.f * scale), ec, name);
    }

    if (v.show_health) {
        // Bar to the left of the box: 3 px wide, 4 px gap, bottom-up, 78%.
        const float bw = 3.f * scale;
        const float gap = 4.f * scale;
        const float bx = box_cx - box_w * 0.5f - gap - bw;
        dl->AddRectFilled(ImVec2(bx, box_top), ImVec2(bx + bw, box_bottom),
                          IM_COL32(0, 0, 0, 150));
        const float fill = box_h * 0.78f;
        dl->AddRectFilled(ImVec2(bx, box_bottom - fill), ImVec2(bx + bw, box_bottom),
                          kHealthBar);
    }

    if (v.show_distance) {
        const char* dist = "42 m";
        const float tw = text_width(dist, 10.f * scale);
        dl->AddText(ImGui::GetFont(), 10.f * scale,
                    ImVec2(box_cx - tw * 0.5f, box_bottom + 7.f * scale), ec, dist);
    }
}

// -----------------------------------------------------------------------------
//  The aim assist preview.  Simulated target and cursor, no live data.
// -----------------------------------------------------------------------------
void draw_aim_preview(ImDrawList* dl, AimAssist& aim, ImVec2 tl, ImVec2 br, float scale) {
    const ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);

    // A fixed simulated target, placed to the upper right of centre so the pull
    // is visible as a direction rather than a magnitude only.
    const ImVec2 target(c.x + 90.f * scale, c.y - 40.f * scale);

    if (!aim.enabled) {
        const char* msg = "Recurso desativado";
        const float tw = text_width(msg, 12.f * scale);
        dl->AddText(ImGui::GetFont(), 12.f * scale,
                    ImVec2(c.x - tw * 0.5f, c.y - 6.f * scale), kTextSecond, msg);
        return;
    }

    // The simulated cursor drifts toward the target at a rate set by smoothing,
    // so the slider visibly does something.  This is the smoothing maths the real
    // assist will use -- kept here so it is validated against a known target
    // before it is ever pointed at a player.
    if (aim.sim.cursor.x == 0.f && aim.sim.cursor.y == 0.f)
        aim.sim.cursor = ImVec2(c.x - 70.f * scale, c.y + 60.f * scale);

    ImGuiIO& io = ImGui::GetIO();
    const float dt = (io.DeltaTime > 0.f && io.DeltaTime < 0.1f) ? io.DeltaTime : 1.f / 60.f;

    const float dx = target.x - aim.sim.cursor.x;
    const float dy = target.y - aim.sim.cursor.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    aim.sim.in_range = dist <= aim.radius_px * scale;

    if (aim.sim.in_range) {
        // Frame-rate independent convergence, same form as the box smoothing:
        // higher "smoothing" means slower, so it divides the exponent.
        const float s = std::max(aim.smoothing, 1.f);
        const float a = 1.f - std::pow(1.f - 1.f / (s * 10.f), dt * 60.f);
        aim.sim.cursor.x += dx * a;
        aim.sim.cursor.y += dy * a;
        aim.sim.pull = std::min(1.f, aim.sim.pull + dt * 3.f);
    } else {
        // Out of range: wander a little so the range test is observable.
        aim.sim.cursor.x += std::sin((float)GetTickCount() * 0.0009f) * 40.f * dt * scale;
        aim.sim.cursor.y += std::cos((float)GetTickCount() * 0.0011f) * 40.f * dt * scale;
        aim.sim.pull = std::max(0.f, aim.sim.pull - dt * 3.f);
    }

    // Selection radius, per spec §10 -- visible only while enabled.
    if (aim.region == 1) {
        dl->AddCircle(aim.sim.cursor, aim.radius_px * scale,
                      IM_COL32(0xB5, 0xB5, 0xBA, 60), 48, 1.f * scale);
    }

    // Target.
    const float tr = 14.f * scale;
    dl->AddCircle(target, tr, kAccent, 32, 1.f * scale);
    dl->AddCircleFilled(target, 3.f * scale, kAccent);

    // Cursor: a small cross, greyer than the target so the two read apart.
    const ImU32 cc = IM_COL32(0xD8, 0xD8, 0xD8, 200);
    dl->AddLine(ImVec2(aim.sim.cursor.x - 7.f * scale, aim.sim.cursor.y),
                ImVec2(aim.sim.cursor.x + 7.f * scale, aim.sim.cursor.y), cc, 1.f * scale);
    dl->AddLine(ImVec2(aim.sim.cursor.x, aim.sim.cursor.y - 7.f * scale),
                ImVec2(aim.sim.cursor.x, aim.sim.cursor.y + 7.f * scale), cc, 1.f * scale);

    // Pull indicator: a line from cursor toward target whose opacity tracks how
    // settled the pull is.
    if (aim.sim.pull > 0.01f) {
        dl->AddLine(aim.sim.cursor, target,
                    IM_COL32(0xB5, 0xB5, 0xBA, (int)(90 * aim.sim.pull)), 1.f * scale);
    }
}

// -----------------------------------------------------------------------------
//  Sidebar.
// -----------------------------------------------------------------------------
void draw_sidebar(ImDrawList* dl, UiState& st, ImVec2 tl, ImVec2 br, float scale) {
    dl->AddRectFilled(tl, br, kSidebarBg, kOuterRound * scale,
                      ImDrawFlags_RoundCornersLeft);
    // Divider between sidebar and content.
    dl->AddLine(ImVec2(br.x, tl.y), ImVec2(br.x, br.y), kDivider, 1.f * scale);

    const ImVec2 logo(tl.x + 32.f * scale, tl.y + 35.f * scale);
    dl->AddRectFilled(ImVec2(logo.x - 15.f * scale, logo.y - 15.f * scale),
                      ImVec2(logo.x + 15.f * scale, logo.y + 15.f * scale), kTabActiveBg, 9.f * scale);
    for (int i = 0; i < 3; ++i) {
        const float y = logo.y + (i - 1) * 5.f * scale;
        dl->AddLine(ImVec2(logo.x - 8.f * scale, y),
                    ImVec2(logo.x + (i == 1 ? 5.f : 9.f) * scale, y), kAccent, 1.5f * scale);
    }
    dl->AddText(ImGui::GetFont(), 16.f * scale,
                ImVec2(tl.x + 56.f * scale, tl.y + 21.f * scale), kTextPrimary, "vex");
    dl->AddText(ImGui::GetFont(), 10.f * scale,
                ImVec2(tl.x + 56.f * scale, tl.y + 43.f * scale), kTextSecond, "Painel de controle");
    text_spaced(dl, ImVec2(tl.x + 20.f * scale, tl.y + 88.f * scale),
                kTextSecond, "RECURSOS", 1.f * scale, 9.f * scale);

    struct Item { Tab tab; const char* label; bool avail; };
    const Item items[] = {
        { Tab::Esp,       "ESP",       true  },
        { Tab::AimAssist, "Aim Assist", true },  // structure only; see the tab body
    };

    float y = tl.y + 111.f * scale;
    for (const Item& it : items) {
        const float h = kTabH * scale;
        ImVec2 a(tl.x + 10.f * scale, y);
        ImVec2 b(br.x - 10.f * scale, y + h);

        // Full-row invisible button for the hit-test.
        ImGui::SetCursorScreenPos(a);
        char id[32];
        snprintf(id, sizeof id, "##tab%d", (int)it.tab);
        ImGui::InvisibleButton(id, ImVec2(b.x - a.x, h));
        const bool hovered = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked();
        if (clicked) st.menu.tab = it.tab;

        const bool active = (st.menu.tab == it.tab);
        if (active) {
            dl->AddRectFilled(a, b, kTabActiveBg, 8.f * scale);
            dl->AddRect(a, b, kCardBorder, 8.f * scale, 0, scale);
            dl->AddRectFilled(ImVec2(a.x + 3.f * scale, a.y + 13.f * scale),
                              ImVec2(a.x + 5.f * scale, b.y - 13.f * scale), kAccent, scale);
        } else if (hovered) {
            dl->AddRectFilled(a, b, IM_COL32(0x1A, 0x1A, 0x1C, 0xFF), 5.f * scale);
        }

        // A simple eye for ESP, a simple dot-and-ring for aim assist: both are a
        // few primitives rather than an icon font, so nothing has to be loaded.
        const float icy = a.y + h * 0.5f;
        const float icx = a.x + 20.f * scale;
        const ImU32 ic = active ? kTextPrimary : kTextSecond;
        if (it.tab == Tab::Esp) {
            dl->PathLineTo(ImVec2(icx - 8.f * scale, icy));
            dl->PathBezierCubicCurveTo(ImVec2(icx - 3.f * scale, icy - 8.f * scale),
                ImVec2(icx + 3.f * scale, icy - 8.f * scale), ImVec2(icx + 8.f * scale, icy));
            dl->PathBezierCubicCurveTo(ImVec2(icx + 3.f * scale, icy + 8.f * scale),
                ImVec2(icx - 3.f * scale, icy + 8.f * scale), ImVec2(icx - 8.f * scale, icy));
            dl->PathStroke(ic, ImDrawFlags_Closed, scale);
            dl->AddCircleFilled(ImVec2(icx, icy), 2.5f * scale, ic);
        } else {
            dl->AddCircle(ImVec2(icx, icy), 6.f * scale, ic, 16, 1.f * scale);
            dl->AddLine(ImVec2(icx - 9.f * scale, icy), ImVec2(icx + 9.f * scale, icy), ic, scale);
            dl->AddLine(ImVec2(icx, icy - 9.f * scale), ImVec2(icx, icy + 9.f * scale), ic, scale);
        }

        dl->AddText(ImGui::GetFont(), 12.f * scale,
                    ImVec2(a.x + 36.f * scale, a.y + (h - 12.f * scale) * 0.5f),
                    active ? kTextPrimary : kTextSecond, it.label);

        y += h + 4.f * scale;
    }
    dl->AddLine(ImVec2(tl.x + 16.f * scale, br.y - 61.f * scale),
                ImVec2(br.x - 16.f * scale, br.y - 61.f * scale), kDivider, scale);
    dl->AddText(ImGui::GetFont(), 10.f * scale,
                ImVec2(tl.x + 20.f * scale, br.y - 40.f * scale), kTextSecond, "Interface / tema escuro");
}

// -----------------------------------------------------------------------------
//  The ESP tab body: header, two-column cards, footer.
// -----------------------------------------------------------------------------
void draw_esp_tab(UiState& st, ImVec2 tl, ImVec2 br, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    EspVisual& v = st.esp;

    // ---- header ----
    dl->AddText(ImGui::GetFont(), 10.f * scale, ImVec2(tl.x, tl.y - 2.f * scale), kTextSecond, "VISUAL / ESP");
    dl->AddText(ImGui::GetFont(), 20.f * scale, ImVec2(tl.x, tl.y + 15.f * scale), kTextPrimary, "ESP");
    dl->AddText(ImGui::GetFont(), 11.f * scale,
                ImVec2(tl.x, tl.y + 43.f * scale), kTextSecond,
                "Personalize os elementos visuais");
    dl->AddLine(ImVec2(tl.x, tl.y + kHeaderH * scale - 12.f * scale),
                ImVec2(br.x, tl.y + kHeaderH * scale - 12.f * scale),
                kDivider, 1.f * scale);

    const float body_top = tl.y + kHeaderH * scale;

    // Two columns, ratio 1 : 0.8.
    const float gap = kCardGap * scale;
    const float avail_w = br.x - tl.x;
    const float col_l = (avail_w - gap) / 1.8f;

    // -----------------------------------------------------------------------
    //  Card heights are COMPUTED from their contents, not written as constants.
    //
    //  The first version of this had things like
    //      geral_h = 16 + 18 + kRowH * 5 * scale + 16
    //  which mixes scaled and unscaled numbers in one expression: the three
    //  literals are device pixels while the rows are DPI-scaled, so the card is
    //  the wrong height at every scale except 1.0 and the content either
    //  overflows the border or floats inside it.  At 150% -- which is the common
    //  Windows default on a laptop -- five rows need 292 px and the constant
    //  reserved 211.
    //
    //  Deriving each height from its own rows means the arithmetic cannot drift:
    //  add a row and the card grows.
    //
    //  Padding constants, once: 14 above the title, 18 below it, 16 at the
    //  bottom, all scaled.
    // -----------------------------------------------------------------------
    const float card_pad_top    = 14.f * scale;
    const float card_pad_below  = 18.f * scale;
    const float card_pad_bottom = 16.f * scale;

    const int   geral_rows = 5;
    const float geral_h = card_pad_top + card_pad_below
                        + kRowH * scale * (float)geral_rows
                        + card_pad_bottom
                        + 14.f * scale;   // the "em desenvolvimento" note

    // APARÊNCIA: title, then colour row, distance row, slider.
    const float aparencia_h = card_pad_top + card_pad_below
                            + 30.f * scale   // colour row
                            + 30.f * scale   // distance value row
                            + 26.f * scale   // slider
                            + card_pad_bottom;

    ImVec2 g_tl(tl.x, body_top);
    ImVec2 g_br(tl.x + col_l, body_top + geral_h);
    card_begin(dl, g_tl, g_br, "GERAL", scale);

    ImGui::SetCursorScreenPos(ImVec2(g_tl.x + 16.f * scale,
                                     g_tl.y + card_pad_top + card_pad_below));
    ImGui::PushID("geral");
    const float controls_right = g_br.x - 16.f * scale;
    switch_row("##en",  "Ativar ESP",   &v.enabled,       scale, true, controls_right);
    switch_row("##bx",  "Caixas",       &v.boxes,         scale, true, controls_right);
    // Names and health are present and disabled: the read path has no measured
    // offset for either, and a switch that does nothing is worth less than a
    // switch that says it does nothing.
    switch_row("##nm",  "Nomes",        &v.show_names,    scale, false, controls_right);
    switch_row("##hp",  "Barra de vida", &v.show_health,  scale, false, controls_right);
    switch_row("##ds",  "Distância",    &v.show_distance, scale, true, controls_right);
    ImGui::PopID();

    // The disabled pair gets one explanation rather than two, placed inside the
    // card and clear of the border.
    dl->AddText(ImGui::GetFont(), 9.f * scale,
                ImVec2(g_tl.x + 16.f * scale,
                       g_br.y - card_pad_bottom - 9.f * scale),
                kTextSecond, "Nomes e vida: em desenvolvimento");

    ImVec2 a_tl(tl.x, g_br.y + gap);
    // The APARÊNCIA card fills the rest of the column rather than ending at its
    // own content height: two left-hand cards of different heights, with the
    // preview beside them, looked like a layout mistake.  aparencia_h is the
    // height the content actually needs, and the card takes the larger of that
    // and whatever is left, so it never clips and never floats short.
    const float col_bottom = br.y - 48.f * scale;
    const float a_h = std::max(aparencia_h, col_bottom - a_tl.y);
    ImVec2 a_br(tl.x + col_l, a_tl.y + a_h);
    card_begin(dl, a_tl, a_br, "APARÊNCIA", scale);

    // The card's content begins below the title, at the same padding the GERAL
    // card uses, and every row is placed at a cursor the helpers advance -- so
    // the layout and the computed card height are two readings of one sequence
    // rather than two independent sums that have to be kept in step by hand.
    float ay = a_tl.y + card_pad_top + card_pad_below;
    ImGui::SetCursorScreenPos(ImVec2(a_tl.x + 16.f * scale, ay));
    ImGui::PushID("aparencia");

    // Colour row: label left, swatch right, on the same baseline.
    value_row("Cor dos elementos", "", scale);
    ImGui::SetCursorScreenPos(ImVec2(a_br.x - 16.f * scale - 20.f * scale,
                                     ay + 4.f * scale));
    color_swatch("##col", v.color, scale);
    ay += 30.f * scale;

    // Distance value + slider.
    char dv[32];
    snprintf(dv, sizeof dv, "%.0f m", v.max_distance);
    ImGui::SetCursorScreenPos(ImVec2(a_tl.x + 16.f * scale, ay));
    value_row("Distância máxima", dv, scale, a_br.x - 16.f * scale);
    ay += 30.f * scale;

    ImGui::SetCursorScreenPos(ImVec2(a_tl.x + 16.f * scale, ay));
    ImGui::SetNextItemWidth(a_br.x - a_tl.x - 32.f * scale);
    thin_slider("##dist", &v.max_distance, 10.f, 500.f, 10.f, "%.0f", scale);
    ImGui::PopID();

    // ---- right column: PRÉVIA ----
    ImVec2 p_tl(tl.x + col_l + gap, body_top);
    ImVec2 p_br(br.x, br.y - 48.f * scale);
    card_begin(dl, p_tl, p_br, "PRÉVIA", scale);

    // "Simulação" on the right of the card title.
    {
        const char* s = "Simulação";
        const float tw = text_width(s, 9.f * scale);
        dl->AddText(ImGui::GetFont(), 9.f * scale,
                    ImVec2(p_br.x - 16.f * scale - tw, p_tl.y + 14.f * scale),
                    kTextSecond, s);
    }

    // The preview surface fills the card, with kPreviewH as the minimum it will
    // ever be.  A fixed height was the first version and it left dead space in
    // the card whenever the window was taller than the reference -- and the
    // window is explicitly allowed to grow.
    const float caption_h = 16.f * scale;
    ImVec2 s_tl(p_tl.x + 16.f * scale, p_tl.y + 30.f * scale);
    float surf_h = (p_br.y - 16.f * scale - caption_h) - s_tl.y;
    if (surf_h < kPreviewH * scale) surf_h = kPreviewH * scale;
    ImVec2 s_br(p_br.x - 16.f * scale, s_tl.y + surf_h);

    dl->AddRectFilled(s_tl, s_br, kPreviewBg, 4.f * scale);
    // A very faint radial glow: three concentric filled circles, decreasing
    // alpha, centred on the figure.  Cheap and it is the whole of the
    // "iluminação radial cinza muito discreta" the spec asks for.
    {
        const ImVec2 c((s_tl.x + s_br.x) * 0.5f, (s_tl.y + s_br.y) * 0.52f);
        const float rmax = std::min(s_br.x - s_tl.x, s_br.y - s_tl.y) * 0.5f;
        for (int i = 4; i >= 1; --i) {
            dl->AddCircleFilled(c, rmax * (float)i / 4.f,
                                IM_COL32(0x26, 0x26, 0x29, (int)(10 + i * 4)), 48);
        }
    }
    dl->AddRect(s_tl, s_br, kPreviewBorder, 4.f * scale, 0, 1.f * scale);
    dl->PushClipRect(s_tl, s_br, true);
    draw_preview_silhouette(dl, s_tl, s_br, scale);
    draw_preview_elements(dl, v, st, s_tl, s_br, scale);
    dl->PopClipRect();

    dl->AddText(ImGui::GetFont(), 9.f * scale,
                ImVec2(s_tl.x, s_br.y + 8.f * scale), kTextSecond,
                "Dados fictícios para visualizar o estilo.");

    // ---- footer ----
    const float fy = br.y - 40.f * scale;
    dl->AddLine(ImVec2(tl.x, fy - 8.f * scale), ImVec2(br.x, fy - 8.f * scale),
                kDivider, 1.f * scale);

    // "Perfil real à esquerda, se disponível."
    //
    // The process has no profile concept and inventing a name would be exactly
    // the fake readout the spec forbids, so the label says the true thing: this
    // build has none.
    dl->AddText(ImGui::GetFont(), 10.f * scale,
                ImVec2(tl.x, fy + 6.f * scale), kTextSecond, "Perfil: --");

    ImGui::SetCursorScreenPos(ImVec2(br.x - 130.f * scale, fy));
    if (footer_button("Restaurar padrão", scale))
        st.menu.reset_requested = true;
}

// -----------------------------------------------------------------------------
//  The aim assist tab body: structure and simulation, no live behaviour.
// -----------------------------------------------------------------------------
void draw_aim_tab(UiState& st, ImVec2 tl, ImVec2 br, float scale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    AimAssist& a = st.aim;

    dl->AddText(ImGui::GetFont(), 16.f * scale, ImVec2(tl.x, tl.y), kTextPrimary, "Aim Assist");
    dl->AddText(ImGui::GetFont(), 11.f * scale,
                ImVec2(tl.x, tl.y + 24.f * scale), kTextSecond,
                "Estrutura e simulação isolada");
    dl->AddLine(ImVec2(tl.x, tl.y + kHeaderH * scale - 12.f * scale),
                ImVec2(br.x, tl.y + kHeaderH * scale - 12.f * scale),
                kDivider, 1.f * scale);

    const float body_top = tl.y + kHeaderH * scale;
    const float gap = kCardGap * scale;
    const float avail_w = br.x - tl.x;
    const float col_l = (avail_w - gap) / 1.8f;

    // Left: configuration.
    ImVec2 l_tl(tl.x, body_top), l_br(tl.x + col_l, br.y - 48.f * scale);
    card_begin(dl, l_tl, l_br, "CONFIGURAÇÃO", scale);

    ImGui::SetCursorScreenPos(ImVec2(l_tl.x + 16.f * scale, l_tl.y + 30.f * scale));
    ImGui::PushID("aim");

    switch_row("##aen", "Ativar", &a.enabled, scale, true, l_br.x - 16.f * scale);

    // Keybind.
    value_row("Tecla de ativação", key_name(a.vk_key).c_str(), scale, l_br.x - 16.f * scale);

    // Radius and smoothing sliders, both disabled with the master switch off --
    // which is what "desabilitá-las claramente" means in practice.
    ImGui::BeginDisabled(!a.enabled);
    {
        char rv[32];
        snprintf(rv, sizeof rv, "%.0f px", a.radius_px);
        value_row("Raio de seleção", rv, scale, l_br.x - 16.f * scale);
        ImGui::SetNextItemWidth(l_br.x - l_tl.x - 32.f * scale);
        thin_slider("##rad", &a.radius_px, 20.f, 400.f, 0.f, "%.0f", scale);

        char sv[32];
        snprintf(sv, sizeof sv, "%.1f", a.smoothing);
        value_row("Suavização", sv, scale, l_br.x - 16.f * scale);
        ImGui::SetNextItemWidth(l_br.x - l_tl.x - 32.f * scale);
        thin_slider("##smo", &a.smoothing, 1.f, 20.f, 0.f, "%.1f", scale);
    }
    ImGui::EndDisabled();
    ImGui::PopID();

    ImGui::SetCursorScreenPos(ImVec2(l_tl.x + 16.f * scale, l_br.y - 40.f * scale));
    dl->AddText(ImGui::GetFont(), 9.f * scale, ImGui::GetCursorScreenPos(),
                kTextSecond, "Movimento real: em desenvolvimento");

    // Right: preview.
    ImVec2 r_tl(tl.x + col_l + gap, body_top), r_br(br.x, br.y - 48.f * scale);
    card_begin(dl, r_tl, r_br, "PRÉVIA", scale);

    ImVec2 s_tl(r_tl.x + 16.f * scale, r_tl.y + 30.f * scale);
    ImVec2 s_br(r_br.x - 16.f * scale, s_tl.y + kPreviewH * scale);
    dl->AddRectFilled(s_tl, s_br, kPreviewBg, 4.f * scale);
    dl->AddRect(s_tl, s_br, kPreviewBorder, 4.f * scale, 0, 1.f * scale);
    dl->PushClipRect(s_tl, s_br, true);
    draw_aim_preview(dl, a, s_tl, s_br, scale);
    dl->PopClipRect();

    dl->AddText(ImGui::GetFont(), 9.f * scale,
                ImVec2(s_tl.x, s_br.y + 8.f * scale), kTextSecond,
                "Alvo e cursor simulados. Nenhum movimento real.");
}

} // namespace

// =============================================================================
//  Public entry points
// =============================================================================

void UiState::restore_defaults() {
    esp = EspVisual{};
    aim = AimAssist{};
    // The menu's own state is not a visual setting and is left alone: resetting
    // the tab or closing the menu on a "restore defaults" press would be
    // surprising.  The spec says visual settings only.
}

void draw_menu(UiState& st) {
    if (!st.menu.open) return;

    const ImGuiIO& display = ImGui::GetIO();
    if (display.DisplaySize.x < 1.f || display.DisplaySize.y < 1.f) return;
    const float scale = std::min(dpi_scale(), std::min(
        std::max(1.f, display.DisplaySize.x - 24.f) / kMenuWidth,
        std::max(1.f, display.DisplaySize.y - 24.f) / kMenuMinH));

    // One window for the whole menu, sized to the reference and DPI-scaled.
    // NoDecoration because the chrome is drawn by hand; NoMove for now so it
    // cannot be dragged off-screen with no way back.
    const float w = kMenuWidth * scale;
    const float h = kMenuMinH * scale;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - w) * 0.5f,
                                   (io.DisplaySize.y - h) * 0.5f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 5.f * scale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, col(kCardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, col(kCardBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, col(kTextPrimary));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col(kButtonBg));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col(kButtonHover));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col(kTabActiveBg));
    ImGui::PushStyleColor(ImGuiCol_Header, col(kTabActiveBg));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, col(kButtonHover));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    if (ImGui::Begin("##bs_menu", nullptr, flags)) {
        ImVec2 tl = ImGui::GetWindowPos();
        ImVec2 sz = ImGui::GetWindowSize();
        ImVec2 br(tl.x + sz.x, tl.y + sz.y);

        st.menu.pos  = tl;
        st.menu.size = sz;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // The menu rectangle and nothing else: the spec is explicit that no
        // opaque backdrop goes over the game.
        dl->AddRectFilled(tl, br, kMenuBg, kOuterRound * scale);
        dl->AddRect(tl, br, kOuterBorder, kOuterRound * scale, 0, 1.f * scale);

        const ImVec2 sb_tl = tl;
        const ImVec2 sb_br(tl.x + kSidebarW * scale, br.y);
        draw_sidebar(dl, st, sb_tl, sb_br, scale);

        const ImVec2 ct_tl(sb_br.x + kContentPad * scale, tl.y + kContentPad * scale);
        const ImVec2 ct_br(br.x - kContentPad * scale, br.y - 14.f * scale);

        // Clip the content so a card can never draw over the sidebar or the
        // rounded outer edge.
        dl->PushClipRect(ct_tl, ImVec2(ct_br.x, br.y - 2.f * scale), true);
        if (st.menu.tab == Tab::Esp)
            draw_esp_tab(st, ct_tl, ct_br, scale);
        else
            draw_aim_tab(st, ct_tl, ct_br, scale);
        dl->PopClipRect();

        const ImVec2 close_pos(br.x - 48.f * scale, tl.y + 18.f * scale);
        ImGui::SetCursorScreenPos(close_pos);
        if (ImGui::InvisibleButton("##close_menu", ImVec2(28.f * scale, 28.f * scale)))
            st.menu.open = false;
        if (ImGui::IsItemHovered())
            dl->AddRectFilled(close_pos, ImVec2(close_pos.x + 28.f * scale,
                close_pos.y + 28.f * scale), kButtonHover, 7.f * scale);
        const ImVec2 c(close_pos.x + 14.f * scale, close_pos.y + 14.f * scale);
        dl->AddLine(ImVec2(c.x - 4.f * scale, c.y - 4.f * scale),
                    ImVec2(c.x + 4.f * scale, c.y + 4.f * scale), kTextSecond, scale);
        dl->AddLine(ImVec2(c.x + 4.f * scale, c.y - 4.f * scale),
                    ImVec2(c.x - 4.f * scale, c.y + 4.f * scale), kTextSecond, scale);
    }
    ImGui::End();

    ImGui::PopStyleColor(9);
    ImGui::PopStyleVar(5);

    if (st.menu.reset_requested) {
        st.restore_defaults();
        st.menu.reset_requested = false;
    }
}

void feed_mouse(UiState& st, bool menu_open, HWND overlay_hwnd) {
    ImGuiIO& io = ImGui::GetIO();

    // -------------------------------------------------------------------------
    //  ClipCursor, released once on open and restored once on close.
    //
    //  The game holds the pointer with ClipCursor so it cannot leave the window
    //  -- which is correct for the game and fatal for a menu drawn inside that
    //  window, because the pointer can never reach the menu's controls.  Releasing
    //  the clip is therefore not optional if the menu is to be usable at all, and
    //  it is the one place this program interferes with the game's mouse.
    //
    //  It is done on the TRANSITION, not every frame: a per-frame ClipCursor(NULL)
    //  is a per-frame perturbation of exactly the state that was shown to break
    //  input, and the game only re-clips in response to input it processes, so
    //  re-releasing every frame would keep the clip off for the whole session.
    //
    //  On close the clip is intentionally NOT restored.  Restoring it requires
    //  the game's clip rect, which is not readable, and calling ClipCursor with a
    //  guessed rect would confine the pointer to the wrong region.  The game
    //  re-asserts its own clip the next time it processes mouse input, which after
    //  closing the menu is immediately.  Doing nothing is both simpler and safer
    //  than a guess.
    // -------------------------------------------------------------------------
    if (menu_open != st.menu.mouse_gate_open) {
        st.menu.mouse_gate_open = menu_open;
        if (menu_open) {
            ClipCursor(nullptr);
            printf("[menu] opened -- released the clip, the pointer is free\n");
        } else {
            printf("[menu] closed -- the game will re-assert its own clip\n");
            // Neutral state: a button held at close must not read as held at
            // reopen, or the first click of the next session is swallowed.
            io.AddMouseButtonEvent(0, false);
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        }
    }

    if (!menu_open) {
        // Menu closed: nothing is fed, so nothing can be clicked.  Only the
        // release above ever touched anything.
        st.menu.mouse_prev_down = false;
        return;
    }

    // -------------------------------------------------------------------------
    //  WHY THE MOUSE IS READ FROM THE OS AND NOT FROM THE WINDOW.
    //
    //  The overlay is a layered, click-through, WS_EX_NOACTIVATE window, and it
    //  has to stay that way -- that is the whole reason the game's input works.
    //  It receives no useful WM_MOUSEMOVE of its own, and it must never call
    //  SetCapture.  What the menu needs is a position and a button state, and
    //  both are available process-wide: GetCursorPos for the position and
    //  GetAsyncKeyState for the button.  Handing those to ImGui as raw input
    //  events is what the backend would do with a message, minus the capture.
    //  ImGuiConfigFlags_NoMouse stays set, so the backend still never touches the
    //  system cursor.
    // -------------------------------------------------------------------------
    POINT p{};
    GetCursorPos(&p);

    // ImGui's coordinate space is the overlay's CLIENT area -- the Win32 backend
    // sets io.DisplaySize from GetClientRect every frame.  So the position has
    // to be converted to client coordinates the same way, and the earlier use of
    // GetWindowRect was one border out: identical for a borderless layered
    // window, wrong the moment anything gives it a frame, and there is no reason
    // to depend on that.
    //
    // ClientToScreen is the correct transform and, like every other call here,
    // it is a read: it does not activate the window, alter the cursor or touch
    // the clip.
    ImVec2 mouse((float)p.x, (float)p.y);
    if (overlay_hwnd) {
        POINT origin{ 0, 0 };
        if (ClientToScreen(overlay_hwnd, &origin)) {
            mouse.x -= (float)origin.x;
            mouse.y -= (float)origin.y;
        }
    }
    io.AddMousePosEvent(mouse.x, mouse.y);

    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (down != st.menu.mouse_prev_down) {
        io.AddMouseButtonEvent(0, down);
        st.menu.mouse_prev_down = down;
    }
}

void load_font(float scale) {
    ImGuiIO& io = ImGui::GetIO();

    // Segoe UI, which is what the spec asks for.  The bold weight is added as a
    // second font so the panel titles can use it; ImGui has no font weight axis,
    // so "semibold" is achieved by choosing the semibold TTF.
    const float base = 13.f * scale;

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.PixelSnapH  = true;

    ImFont* regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", base, &cfg);
    if (!regular) {
        // Fallback: the default font, so a machine without that path still gets
        // a working menu rather than an assertion.
        io.Fonts->AddFontDefault();
    }

    ImFontConfig sb = cfg;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", base, &sb))
        io.Fonts->AddFontDefault();

    // Small face for the 9-10 px labels.  Rasterised at its own size rather than
    // scaled down from 13, because a downscaled bitmap font is the one case where
    // ImGui text looks genuinely bad.
    ImFontConfig sm = cfg;
    if (!io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 10.f * scale, &sm))
        io.Fonts->AddFontDefault();
}

float dpi_scale() {
    // The overlay window lives on the game's monitor.  GetDpiForSystem is used
    // because constructing a temporary window for GetDpiForWindow on every call
    // would be worse than a correct-enough system value; the game is normally on
    // the primary monitor, which is what this reports.
    static float cached = 0.f;
    if (cached > 0.f) return cached;

    HDC dc = GetDC(nullptr);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    if (dpi <= 0) dpi = 96;

    cached = (float)dpi / 96.f;
    // Clamp to a sane band: a misreported DPI would otherwise produce a menu
    // measured in metres or a single unreadable pixel.
    if (cached < 0.75f) cached = 0.75f;
    if (cached > 3.0f)  cached = 3.0f;
    return cached;
}

std::string key_name(int vk) {
    // Named keys first, because GetKeyNameText on a plain letter returns the
    // letter and that is all anyone wants to read.
    switch (vk) {
        case VK_LBUTTON:  return "Mouse 1";
        case VK_RBUTTON:  return "Mouse 2";
        case VK_MBUTTON:  return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_SHIFT:    return "Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_MENU:     return "Alt";
        case VK_CAPITAL:  return "Caps Lock";
        case VK_SPACE:    return "Space";
        default: break;
    }

    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    if (vk >= '0' && vk <= '9') return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) {
        char b[8];
        snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1);
        return b;
    }

    char b[16];
    snprintf(b, sizeof b, "0x%02X", vk);
    return b;
}

} // namespace bs
} // namespace vex
