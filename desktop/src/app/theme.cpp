// Theme presets, ImGui style mapping and JSON files.
// Ported from weatherDesktop (MIT, Cody Savage): presets and apply().
// Ported from the k-os client (same author), relicensed MIT: field tables,
// set_color, JSON persistence.
#include "app/theme.h"

#include "core/types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>

namespace ks::app {

using theme::hex;

Theme Theme::dark_default() {
    Theme t;
    t.name = "Korra Dark";
    t.bg = hex(0x1B1613);
    t.panel = hex(0x241712);
    t.fg = hex(0xE6D6C4);
    t.dim = hex(0x8A5D3D);
    t.amber = hex(0xE07942);
    t.muted = hex(0x5C3F29);
    t.accent = hex(0xE0A773);
    t.check = t.amber;
    t.live = hex(0x6AD84A);
    t.error = hex(0xE0564A);
    t.warn = hex(0xE0B84A);
    t.info = hex(0x9AB4D8);
    t.surface = hex(0x2A1A13);
    t.surface_h = hex(0x382318);
    t.surface_a = hex(0x42291C);
    t.button_h = hex(0x6E4A30);
    t.bezel = hex(0x282422);
    return t;
}

Theme Theme::light_default() {
    // Warm "paper" light theme. It keeps the brand hues but inverts value.
    Theme t;
    t.name = "Paper Light";
    t.bg = hex(0xF3ECE2);
    t.panel = hex(0xFBF6EE);
    t.fg = hex(0x2A211B);
    t.dim = hex(0x8A6A4A);
    t.amber = hex(0xC85A1E);
    t.muted = hex(0xCBB79E);
    t.accent = hex(0x9A5A2A);
    t.check = t.amber;
    t.live = hex(0x2E8B2E);
    t.error = hex(0xC0392B);
    t.warn = hex(0xB07A10);
    t.info = hex(0x3A6EA8);
    t.surface = hex(0xEAE0D2);
    t.surface_h = hex(0xE0D3C0);
    t.surface_a = hex(0xD4C3AC);
    t.button_h = hex(0xE6D8C4);
    t.bezel = hex(0x4A3F36);
    return t;
}

Theme Theme::korra_default() {
    // The k-os Korra palette: chocolate browns, green "eyes" for checks.
    Theme t;
    t.name = "Korra (k-os)";
    t.bg = {0.13f, 0.09f, 0.07f, 1};
    t.panel = {0.16f, 0.12f, 0.09f, 1};
    t.fg = {0.90f, 0.84f, 0.77f, 1};
    t.dim = {0.50f, 0.38f, 0.30f, 1};
    t.amber = {0.88f, 0.47f, 0.26f, 1};
    t.muted = {0.53f, 0.30f, 0.20f, 1};
    t.accent = {0.88f, 0.65f, 0.45f, 1};
    t.check = {0.13f, 0.61f, 0.02f, 1};
    t.live = {0.13f, 0.61f, 0.02f, 1};
    t.warn = {0.88f, 0.72f, 0.26f, 1};
    t.error = {0.80f, 0.25f, 0.20f, 1};
    t.info = {0.60f, 0.50f, 0.80f, 1};
    t.surface = {0.19f, 0.14f, 0.10f, 1};
    t.surface_h = {0.24f, 0.17f, 0.13f, 1};
    t.surface_a = {0.29f, 0.20f, 0.15f, 1};
    t.button_h = {0.67f, 0.47f, 0.37f, 1};
    t.bezel = hex(0x282422);
    t.window_rounding = 4;
    t.frame_rounding = 3;
    return t;
}

bool Theme::is_light() const { return 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z > 0.5f; }

void Theme::apply(float scale) const {
    theme::Bg = bg;
    theme::Panel = panel;
    theme::Fg = fg;
    theme::Dim = dim;
    theme::Amber = amber;
    theme::Muted = muted;
    theme::Accent = accent;
    theme::Live = live;
    theme::Error = error;
    theme::Warn = warn;
    theme::Info = info;
    theme::Surface = surface;
    theme::SurfaceH = surface_h;
    theme::SurfaceA = surface_a;
    theme::ButtonH = button_h;
    theme::Paper = paper;
    theme::PaperInk = paper_ink;
    theme::PaperHead = paper_head;
    theme::Bezel = bezel;
    theme::Light = is_light();

    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = fg;
    c[ImGuiCol_TextDisabled] = dim;
    c[ImGuiCol_WindowBg] = bg;
    c[ImGuiCol_ChildBg] = bg;
    c[ImGuiCol_PopupBg] = panel;
    c[ImGuiCol_Border] = muted;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = surface;
    c[ImGuiCol_FrameBgHovered] = surface_h;
    c[ImGuiCol_FrameBgActive] = surface_a;
    c[ImGuiCol_TitleBg] = bg;
    c[ImGuiCol_TitleBgActive] = bg;
    c[ImGuiCol_TitleBgCollapsed] = bg;
    c[ImGuiCol_MenuBarBg] = panel;
    c[ImGuiCol_ScrollbarBg] = bg;
    c[ImGuiCol_ScrollbarGrab] = muted;
    c[ImGuiCol_ScrollbarGrabHovered] = dim;
    c[ImGuiCol_ScrollbarGrabActive] = amber;
    c[ImGuiCol_CheckMark] = check;
    c[ImGuiCol_SliderGrab] = amber;
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = muted;
    c[ImGuiCol_ButtonHovered] = button_h;
    c[ImGuiCol_ButtonActive] = dim;
    c[ImGuiCol_Header] = surface_h;
    c[ImGuiCol_HeaderHovered] = muted;
    c[ImGuiCol_HeaderActive] = dim;
    c[ImGuiCol_Separator] = muted;
    c[ImGuiCol_SeparatorHovered] = amber;
    c[ImGuiCol_SeparatorActive] = amber;
    c[ImGuiCol_ResizeGrip] = muted;
    c[ImGuiCol_ResizeGripHovered] = dim;
    c[ImGuiCol_ResizeGripActive] = amber;
    // weatherDesktop left the tabs at the ImGui defaults. Set them here.
    c[ImGuiCol_Tab] = surface;
    c[ImGuiCol_TabHovered] = button_h;
    c[ImGuiCol_TabSelected] = surface_a;
    c[ImGuiCol_TabSelectedOverline] = amber;
    c[ImGuiCol_TabDimmed] = surface;
    c[ImGuiCol_TabDimmedSelected] = surface_h;
    c[ImGuiCol_TabDimmedSelectedOverline] = muted;
    c[ImGuiCol_TableHeaderBg] = panel;
    c[ImGuiCol_TableBorderStrong] = muted;
    c[ImGuiCol_TableBorderLight] = surface_h;
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = theme::alpha(amber, 0.05f);
    c[ImGuiCol_PlotHistogram] = amber;
    c[ImGuiCol_PlotHistogramHovered] = accent;
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_TextSelectedBg] = theme::alpha(amber, 0.35f);
    c[ImGuiCol_NavCursor] = amber;
    c[ImGuiCol_ModalWindowDimBg] = theme::alpha(bg, 0.6f);
    // Slots newer than weatherDesktop's ImGui. Without these they keep the
    // ImGui blue.
    c[ImGuiCol_CheckboxSelectedBg] = surface_a;
    c[ImGuiCol_InputTextCursor] = amber;
    c[ImGuiCol_PlotLinesHovered] = amber;
    c[ImGuiCol_TextLink] = amber;
    c[ImGuiCol_TreeLines] = muted;
    c[ImGuiCol_DragDropTarget] = amber;
    c[ImGuiCol_DragDropTargetBg] = theme::alpha(amber, 0.15f);
    c[ImGuiCol_UnsavedMarker] = amber;
    c[ImGuiCol_NavWindowingHighlight] = theme::alpha(amber, 0.7f);
    c[ImGuiCol_NavWindowingDimBg] = theme::alpha(bg, 0.5f);

    s.WindowRounding = window_rounding;
    s.ChildRounding = child_rounding;
    s.FrameRounding = frame_rounding;
    s.PopupRounding = popup_rounding;
    s.GrabRounding = grab_rounding;
    s.ScrollbarRounding = scrollbar_rounding;
    s.TabRounding = tab_rounding;
    s.WindowBorderSize = window_border;
    s.ChildBorderSize = child_border;
    s.FrameBorderSize = frame_border;
    s.WindowPadding = window_padding;
    s.FramePadding = frame_padding;
    s.ItemSpacing = item_spacing;
    s.ItemInnerSpacing = item_inner_spacing;
    s.ScrollbarSize = scrollbar_size;
    s.GrabMinSize = grab_min_size;
    if (scale < 0.95f || scale > 1.05f) s.ScaleAllSizes(scale);

    // Keep the font scale factors. fonts.cpp owns them.
    const ImGuiStyle& old = ImGui::GetStyle();
    s.FontSizeBase = old.FontSizeBase;
    s.FontScaleMain = old.FontScaleMain;
    s.FontScaleDpi = old.FontScaleDpi;
    ImGui::GetStyle() = s;
}

// ---------------------------------------------------------- field tables

#define F(field, label, group) {#field, label, group, &Theme::field}
static const Theme::ColorField s_color_fields[] = {
    F(bg, "background", "palette"),       F(panel, "panel", "palette"),
    F(fg, "text", "palette"),             F(dim, "dim text", "palette"),
    F(amber, "accent", "palette"),        F(muted, "border / button", "palette"),
    F(accent, "heading", "palette"),      F(check, "check mark", "palette"),
    F(surface, "frame", "surfaces"),      F(surface_h, "frame hover", "surfaces"),
    F(surface_a, "frame active", "surfaces"), F(button_h, "button hover", "surfaces"),
    F(live, "good", "status"),            F(warn, "warning", "status"),
    F(error, "error", "status"),          F(info, "info", "status"),
    F(paper, "paper", "device preview"),  F(paper_ink, "ink", "device preview"),
    F(paper_head, "heading ink", "device preview"), F(bezel, "bezel", "device preview"),
    {nullptr, nullptr, nullptr, nullptr}};
#undef F

#define F(field, label, lo, hi) {#field, label, &Theme::field, lo, hi}
static const Theme::FloatField s_float_fields[] = {
    F(window_rounding, "window rounding", 0, 14),  F(child_rounding, "child rounding", 0, 14),
    F(frame_rounding, "frame rounding", 0, 12),    F(popup_rounding, "popup rounding", 0, 12),
    F(grab_rounding, "grab rounding", 0, 12),      F(scrollbar_rounding, "scrollbar rounding", 0, 12),
    F(tab_rounding, "tab rounding", 0, 12),        F(window_border, "window border", 0, 2),
    F(child_border, "child border", 0, 2),         F(frame_border, "frame border", 0, 2),
    F(scrollbar_size, "scrollbar size", 6, 24),    F(grab_min_size, "grab min size", 6, 24),
    {nullptr, nullptr, nullptr, 0, 0}};
static const Theme::Vec2Field s_vec2_fields[] = {
    F(window_padding, "window padding", 0, 30),    F(frame_padding, "frame padding", 0, 20),
    F(item_spacing, "item spacing", 0, 20),        F(item_inner_spacing, "inner spacing", 0, 20),
    {nullptr, nullptr, nullptr, 0, 0}};
#undef F

const Theme::ColorField* Theme::color_fields() { return s_color_fields; }
const Theme::FloatField* Theme::float_fields() { return s_float_fields; }
const Theme::Vec2Field* Theme::vec2_fields() { return s_vec2_fields; }

int Theme::color_field_count() {
    return static_cast<int>(sizeof(s_color_fields) / sizeof(s_color_fields[0])) - 1;
}

bool Theme::set_color(const std::string& key, const Color4& c) {
    for (const auto* f = s_color_fields; f->name; ++f) {
        if (key != f->name) continue;
        this->*(f->ptr) = c;
        return true;
    }
    return false;
}

// ---------------------------------------------------------- JSON

using nlohmann::json;

std::string Theme::to_json() const {
    json j;
    j["name"] = name;
    json& col = j["colors"];
    for (const auto* f = s_color_fields; f->name; ++f) {
        const Color4& c = this->*(f->ptr);
        col[f->name] = {c.x, c.y, c.z, c.w};
    }
    json& g = j["geometry"];
    for (const auto* f = s_float_fields; f->name; ++f) g[f->name] = this->*(f->ptr);
    for (const auto* f = s_vec2_fields; f->name; ++f) g[f->name] = {(this->*(f->ptr)).x, (this->*(f->ptr)).y};
    return j.dump(2);
}

namespace {

bool readColor(const json& v, Color4& out) {
    if (!v.is_array() || v.size() < 3) return false;
    float p[4] = {0, 0, 0, 1};
    for (size_t i = 0; i < v.size() && i < 4; ++i) {
        if (!v[i].is_number()) return false;
        p[i] = std::clamp(v[i].get<float>(), 0.f, 1.f);
    }
    out = Color4(p[0], p[1], p[2], p[3]);
    return true;
}

} // namespace

bool Theme::from_json(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    *this = dark_default();
    if (j.contains("name") && j["name"].is_string()) name = j["name"].get<std::string>();
    // The k-os files keep the colors at the top level. Accept both forms.
    const json& col = j.contains("colors") && j["colors"].is_object() ? j["colors"] : j;
    for (auto it = col.begin(); it != col.end(); ++it) {
        Color4 c;
        if (readColor(it.value(), c)) set_color(it.key(), c);
    }
    if (j.contains("geometry") && j["geometry"].is_object()) {
        const json& g = j["geometry"];
        for (const auto* f = s_float_fields; f->name; ++f)
            if (g.contains(f->name) && g[f->name].is_number())
                this->*(f->ptr) = std::clamp(g[f->name].get<float>(), f->lo, f->hi);
        for (const auto* f = s_vec2_fields; f->name; ++f) {
            const json& v = g.contains(f->name) ? g[f->name] : json();
            if (v.is_array() && v.size() == 2 && v[0].is_number() && v[1].is_number())
                this->*(f->ptr) = ImVec2(std::clamp(v[0].get<float>(), f->lo, f->hi),
                                         std::clamp(v[1].get<float>(), f->lo, f->hi));
        }
    }
    return true;
}

bool Theme::save_to_file(const std::string& path) const { return writeFile(path, to_json()); }

bool Theme::load_from_file(const std::string& path) {
    bool ok = false;
    std::string text = readFile(path, &ok);
    return ok && from_json(text);
}

// ---------------------------------------------------------- theme files

std::string themesDir() { return joinPath(dataDir(), "themes"); }

std::string customThemePath() { return joinPath(dataDir(), "ui_theme.json"); }

std::vector<ThemeFile> scanThemeFiles() {
    std::vector<ThemeFile> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::u8path(themesDir());
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || toLower(it->path().extension().u8string()) != ".json") continue;
        ThemeFile tf;
        tf.path = it->path().u8string();
        tf.name = it->path().stem().u8string();
        Theme t;
        if (t.load_from_file(tf.path) && !t.name.empty()) tf.name = t.name;
        out.push_back(std::move(tf));
    }
    std::sort(out.begin(), out.end(), [](const ThemeFile& a, const ThemeFile& b) { return a.name < b.name; });
    return out;
}

} // namespace ks::app
