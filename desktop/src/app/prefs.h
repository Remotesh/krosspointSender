// UI preferences: theme, fonts, scale, motion, geometry, shortcuts.
// They live in <data>/ui_prefs.json. The app settings (net::saveSettings)
// stay separate: they are shared with the CLI and the helper server.
#pragma once

#include "app/keybinds.h"

#include <array>
#include <string>

namespace ks::app {

struct WindowGeom {
    int x = -1, y = -1;   // -1: centered
    int w = 1440, h = 900;
    bool maximized = false;
};

struct UiPrefs {
    std::string theme_preset = "dark";   // dark | light | korra | custom | file:<path>
    float font_scale = 1.0f;             // 0.5 .. 3.0
    std::string custom_font_path;        // a .ttf/.otf, empty = built-in choice
    bool crisp_font = true;              // Cascadia Mono / Consolas when present
    bool reduce_motion = false;
    bool show_tooltips = true;
    float toast_seconds = 4.0f;          // 1 .. 15
    int discovery_timeout_ms = 1500;     // 300 .. 10000
    // Main columns. A width of 0 for the right column means "the default".
    float left_width = 380.f;            // 220 .. 4000
    float right_width = 0.f;             // 0, or 220 .. 4000
    bool left_collapsed = false;
    bool right_collapsed = false;
    bool device_fit = true;              // device preview: fit (true) or 100 % (false)
    WindowGeom window;
    std::array<int, kActionCount> keybinds{};

    UiPrefs();
    void clamp();
    void resetAppearance();
    void resetShortcuts();
    void resetSystem();
};

std::string prefsPath();   // <data>/ui_prefs.json
UiPrefs loadPrefs();       // defaults when the file is missing or bad
bool savePrefs(const UiPrefs& p);

} // namespace ks::app
