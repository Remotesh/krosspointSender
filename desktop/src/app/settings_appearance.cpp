// Settings: Appearance and Keyboard Shortcuts.
// Ported from the k-os client (same author), relicensed MIT: the UI scale
// presets, the custom font picker, the theme preset combo with user theme
// files, import/export, the color editors (now generic over
// Theme::color_fields()) and the shortcut table.
#include "app/settings_ui.h"

#include <cmath>
#include <filesystem>

namespace ks::app::settings {

namespace {

void scaleRow(App& app) {
    ImGui::TextUnformatted("ui scale");
    const float kPresets[] = {1.00f, 1.25f, 1.50f, 1.75f, 2.00f};
    const char* kLabels[] = {"100%", "125%", "150%", "175%", "200%"};
    bool changed = false;
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine();
        bool active = std::fabs(app.prefs.font_scale - kPresets[i]) < 0.01f;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::Amber);
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Bg);
        }
        if (ImGui::Button(kLabels[i])) {
            app.prefs.font_scale = kPresets[i];
            changed = true;
        }
        if (active) ImGui::PopStyleColor(2);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.f);
    float v = app.prefs.font_scale;
    if (ImGui::SliderFloat("##scale", &v, 0.5f, 3.0f, "%.2fx")) {
        app.prefs.font_scale = v;
        changed = true;
    }
    ui::Tip("scales the text and every size. ctrl+click to type a value.");
    if (changed) {
        app.prefs.clamp();
        app.styleDirty = true;
        app.markPrefsChanged();
    }
}

void fontRow(App& app) {
    ImGui::TextUnformatted("font");
    ImGui::SameLine();
    ImGui::TextDisabled("(now: %s)", app.fontName.c_str());
    if (ImGui::Checkbox("crisp system font (cascadia mono, consolas)", &app.prefs.crisp_font)) {
        app.fontsDirty = true;
        app.markPrefsChanged();
    }
    ui::Tip("off: the classic built-in bitmap font");
    ImGui::TextDisabled("custom font:");
    ImGui::SameLine();
    if (app.prefs.custom_font_path.empty()) ImGui::TextDisabled("none");
    else ImGui::TextWrapped("%s", app.prefs.custom_font_path.c_str());
    if (ImGui::SmallButton("browse...")) {
        if (auto p = openFileDialogFiltered("Choose a font", "Font files", "*.ttf;*.otf")) {
            app.prefs.custom_font_path = *p;
            app.fontsDirty = true;
            app.markPrefsChanged();
        }
    }
    if (!app.prefs.custom_font_path.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset font")) {
            app.prefs.custom_font_path.clear();
            app.fontsDirty = true;
            app.markPrefsChanged();
        }
    }
}

std::string presetLabel(const App& app) {
    const std::string& p = app.prefs.theme_preset;
    if (p == "dark") return "korra dark (default)";
    if (p == "light") return "paper light";
    if (p == "korra") return "korra (k-os)";
    if (p == "custom") return "custom (ui_theme.json)";
    for (auto& f : app.themeFiles)
        if ("file:" + f.path == p) return f.name;
    return p;
}

void themeCombo(App& app) {
    if (!app.themeFilesScanned) {
        app.themeFiles = scanThemeFiles();
        app.themeFilesScanned = true;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.f);
    std::string cur = presetLabel(app);
    if (ImGui::BeginCombo("theme", cur.c_str())) {
        struct Item {
            std::string label, preset;
        };
        std::vector<Item> items = {{"korra dark (default)", "dark"}, {"paper light", "light"}, {"korra (k-os)", "korra"}};
        for (auto& f : app.themeFiles) items.push_back({f.name + "##" + f.path, "file:" + f.path});
        items.push_back({"custom (ui_theme.json)", "custom"});
        for (auto& it : items) {
            bool sel = it.preset == app.prefs.theme_preset;
            if (ImGui::Selectable(it.label.c_str(), sel)) app.applyPreset(it.preset);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::SmallButton("import...")) {
        if (auto src = openFileDialogFiltered("Import theme", "Theme files", "*.json")) {
            namespace fs = std::filesystem;
            makeDirs(themesDir());
            fs::path dst = fs::u8path(themesDir()) / fs::u8path(*src).filename();
            std::error_code ec;
            fs::copy_file(fs::u8path(*src), dst, fs::copy_options::overwrite_existing, ec);
            Theme t;
            if (!ec && t.load_from_file(dst.u8string())) {
                app.applyPreset("file:" + dst.u8string());
                app.themeFilesScanned = false;
                app.toast(ToastKind::Info, "imported theme: " + app.theme.name);
            } else {
                app.toast(ToastKind::Error, "theme import failed");
            }
        }
    }
    ui::Tip("copy a theme .json into the themes folder and use it");
    ImGui::SameLine();
    if (ImGui::SmallButton("export...")) {
        std::string name = app.theme.name.empty() ? "my-theme" : app.theme.name;
        for (auto& c : name)
            if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"') c = '_';
        makeDirs(themesDir());
        if (auto dst = saveFileDialogFiltered("Export theme", "Theme files", "*.json",
                                              joinPath(themesDir(), name + ".json"))) {
            if (app.theme.save_to_file(*dst)) {
                app.themeFilesScanned = false;
                app.toast(ToastKind::Info, "exported theme: " + *dst);
            } else {
                app.toast(ToastKind::Error, "cannot write " + *dst);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("rescan")) app.themeFilesScanned = false;
    ImGui::SameLine();
    if (ImGui::SmallButton("open folder")) {
        makeDirs(themesDir());
        openFolder(themesDir());
    }
}

// Mark the theme as edited: it becomes the "custom" preset.
void themeEdited(App& app) {
    if (app.prefs.theme_preset != "custom") {
        app.prefs.theme_preset = "custom";
        app.theme.name = "Custom";
    }
    app.customThemeDirty = true;
    app.styleDirty = true;
    app.markPrefsChanged();
}

void colorEditors(App& app) {
    const char* group = nullptr;
    bool open = false;
    for (const auto* f = Theme::color_fields(); f->name; ++f) {
        if (!group || std::string(group) != f->group) {
            if (open) ImGui::TreePop();
            group = f->group;
            open = ImGui::TreeNode(group);
        }
        if (!open) continue;
        if (ImGui::ColorEdit4(f->label, &(app.theme.*(f->ptr)).x, ImGuiColorEditFlags_AlphaBar))
            themeEdited(app);
    }
    if (open) ImGui::TreePop();
    if (ImGui::TreeNode("geometry")) {
        ImGui::PushItemWidth(ImGui::GetFontSize() * 12.f);
        for (const auto* f = Theme::float_fields(); f->name; ++f)
            if (ImGui::SliderFloat(f->label, &(app.theme.*(f->ptr)), f->lo, f->hi, "%.1f")) themeEdited(app);
        for (const auto* f = Theme::vec2_fields(); f->name; ++f)
            if (ImGui::SliderFloat2(f->label, &(app.theme.*(f->ptr)).x, f->lo, f->hi, "%.0f")) themeEdited(app);
        ImGui::PopItemWidth();
        ImGui::TreePop();
    }
}

} // namespace

void drawAppearance(App& app) {
    if (!header("Appearance", "theme font scale color preset ui rounding padding dark light korra crisp import export",
                ImGuiTreeNodeFlags_DefaultOpen))
        return;
    if (resetButton("appearance")) {
        app.prefs.resetAppearance();
        app.applyPreset("dark");
        app.fontsDirty = true;
        app.toast(ToastKind::Info, "appearance reset to defaults");
    }
    scaleRow(app);
    ImGui::Spacing();
    fontRow(app);
    ImGui::Spacing();
    themeCombo(app);
    ImGui::Spacing();
    ImGui::TextDisabled("colors and geometry (an edit makes the theme \"custom\")");
    colorEditors(app);
    ImGui::Spacing();
}

void drawShortcuts(App& app) {
    if (!header("Keyboard Shortcuts", "keys keybind hotkey shortcut ctrl binding")) return;
    if (resetButton("shortcuts")) {
        app.prefs.resetShortcuts();
        app.markPrefsChanged();
        app.toast(ToastKind::Info, "shortcuts reset to defaults");
    }
    ui::Hint("Binds with Ctrl or Alt also work while a text box has focus, except the text editing keys.");
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableSetupColumn("bind", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        float w = ImGui::GetFontSize() * 10.f;
        for (int i = 0; i < kActionCount; ++i) {
            auto a = static_cast<Action>(i);
            int& bind = app.prefs.keybinds[static_cast<size_t>(i)];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(actionLabel(a));
            // Show a clash with an earlier action.
            for (int j = 0; j < kActionCount; ++j) {
                if (j == i || bind == 0 || app.prefs.keybinds[static_cast<size_t>(j)] != bind) continue;
                ImGui::SameLine();
                ImGui::TextColored(theme::Warn, "(clash)");
                ui::Tip(actionLabel(static_cast<Action>(j)));
                break;
            }
            ImGui::TableNextColumn();
            if (shortcutKeybindWidget(actionId(a), bind, w)) app.markPrefsChanged();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
}

} // namespace ks::app::settings
