// Theme: named colors, presets, ImGui style, JSON files.
// Ported from weatherDesktop (MIT, Cody Savage): the Korra palette, the
// light "paper" preset, and the ImGuiCol / geometry mapping.
// Ported from the k-os client (same author), relicensed MIT: the Theme
// struct with named fields, the color_fields() table, set_color, presets,
// and JSON save/load (now with nlohmann json and the geometry values).
#pragma once

#include <imgui.h>

#include <string>
#include <vector>

namespace ks::app {

using Color4 = ImVec4;

struct Theme {
    std::string name = "Korra Dark";

    // Palette (weatherDesktop names).
    Color4 bg = {0, 0, 0, 1};        // window background
    Color4 panel = {0, 0, 0, 1};     // popups, menu bar, table headers
    Color4 fg = {1, 1, 1, 1};        // text
    Color4 dim = {0.5f, 0.5f, 0.5f, 1};   // disabled and hint text
    Color4 amber = {1, 0.5f, 0.2f, 1};    // accent, primary action
    Color4 muted = {0.4f, 0.3f, 0.2f, 1}; // borders, buttons
    Color4 accent = {0.9f, 0.7f, 0.4f, 1}; // headings
    Color4 check = {1, 0.5f, 0.2f, 1};    // check marks

    // Surfaces.
    Color4 surface = {0, 0, 0, 1};    // frames
    Color4 surface_h = {0, 0, 0, 1};  // frame hover, headers
    Color4 surface_a = {0, 0, 0, 1};  // frame active
    Color4 button_h = {0, 0, 0, 1};   // button hover

    // Status.
    Color4 live = {0.4f, 0.8f, 0.3f, 1};   // good, online
    Color4 warn = {0.9f, 0.7f, 0.3f, 1};   // warning
    Color4 error = {0.9f, 0.3f, 0.3f, 1};  // error
    Color4 info = {0.6f, 0.7f, 0.9f, 1};   // information

    // Device preview. The paper stays cream in every preset.
    Color4 paper = {0.95f, 0.94f, 0.91f, 1};
    Color4 paper_ink = {0.08f, 0.07f, 0.06f, 1};
    Color4 paper_head = {0.15f, 0.12f, 0.10f, 1};
    Color4 bezel = {0.16f, 0.14f, 0.13f, 1};

    // Geometry (unscaled pixels).
    float window_rounding = 5, child_rounding = 4, frame_rounding = 3, popup_rounding = 4;
    float grab_rounding = 3, scrollbar_rounding = 4, tab_rounding = 3;
    float window_border = 1, child_border = 1, frame_border = 1;
    ImVec2 window_padding = {16, 14};
    ImVec2 frame_padding = {9, 6};
    ImVec2 item_spacing = {9, 9};
    ImVec2 item_inner_spacing = {7, 6};
    float scrollbar_size = 13, grab_min_size = 12;

    static Theme dark_default();    // weatherDesktop Korra dark
    static Theme light_default();   // weatherDesktop paper light
    static Theme korra_default();   // k-os Korra (green check marks)

    // True when the background is light. The title bar tint uses it.
    bool is_light() const;

    // Install the colors and the geometry. scale multiplies the sizes.
    void apply(float scale) const;

    bool save_to_file(const std::string& path) const;
    bool load_from_file(const std::string& path);   // starts from dark_default()
    std::string to_json() const;
    bool from_json(const std::string& text);

    struct ColorField {
        const char* name;    // JSON key
        const char* label;   // UI label
        const char* group;   // UI group
        Color4 Theme::*ptr;
    };
    static const ColorField* color_fields();   // null-terminated
    static int color_field_count();
    bool set_color(const std::string& key, const Color4& c);

    struct FloatField {
        const char* name;
        const char* label;
        float Theme::*ptr;
        float lo, hi;
    };
    static const FloatField* float_fields();   // null-terminated
    struct Vec2Field {
        const char* name;
        const char* label;
        ImVec2 Theme::*ptr;
        float lo, hi;
    };
    static const Vec2Field* vec2_fields();     // null-terminated
};

// The current palette. apply() writes these. The panels read them each
// frame, so a theme switch is only a re-apply (weatherDesktop design).
namespace theme {
inline ImVec4 Bg, Panel, Fg, Dim, Amber, Muted, Accent, Live, Error, Warn, Info;
inline ImVec4 Surface, SurfaceH, SurfaceA, ButtonH;
inline ImVec4 Paper, PaperInk, PaperHead, Bezel;
inline bool Light = false;

// 0xRRGGBB to ImVec4.
inline ImVec4 hex(unsigned v, float a = 1.0f) {
    return ImVec4(static_cast<float>((v >> 16) & 0xFF) / 255.0f, static_cast<float>((v >> 8) & 0xFF) / 255.0f,
                  static_cast<float>(v & 0xFF) / 255.0f, a);
}
inline ImVec4 alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }
} // namespace theme

// User theme files: <data>/themes/*.json.
struct ThemeFile {
    std::string name;
    std::string path;
};
std::string themesDir();
std::vector<ThemeFile> scanThemeFiles();
std::string customThemePath();   // <data>/ui_theme.json

} // namespace ks::app
