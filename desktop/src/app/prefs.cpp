// UI preferences: defaults, clamp, JSON load and save.
#include "app/prefs.h"

#include "core/types.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace ks::app {

using nlohmann::json;

UiPrefs::UiPrefs() { resetShortcuts(); }

void UiPrefs::resetAppearance() {
    UiPrefs d;
    theme_preset = d.theme_preset;
    font_scale = d.font_scale;
    custom_font_path.clear();
    crisp_font = d.crisp_font;
}

void UiPrefs::resetShortcuts() {
    for (int i = 0; i < kActionCount; ++i) keybinds[static_cast<size_t>(i)] = defaultBind(static_cast<Action>(i));
}

void UiPrefs::resetSystem() {
    reduce_motion = false;
    show_tooltips = true;
    toast_seconds = 4.0f;
}

void UiPrefs::clamp() {
    font_scale = std::clamp(font_scale, 0.5f, 3.0f);
    toast_seconds = std::clamp(toast_seconds, 1.0f, 15.0f);
    discovery_timeout_ms = std::clamp(discovery_timeout_ms, 300, 10000);
    // A NaN fails every compare. Then the default comes back.
    if (left_width != left_width) left_width = 380.f;
    left_width = std::clamp(left_width, 220.f, 4000.f);
    if (right_width != right_width || right_width < 0.f) right_width = 0.f;
    if (right_width != 0.f) right_width = std::clamp(right_width, 220.f, 4000.f);
    window.w = std::clamp(window.w, 640, 10000);
    window.h = std::clamp(window.h, 480, 10000);
    if (window.x < -20000 || window.x > 20000) window.x = -1;
    if (window.y < -20000 || window.y > 20000) window.y = -1;
    bool known = theme_preset == "dark" || theme_preset == "light" || theme_preset == "korra" ||
                 theme_preset == "custom" || theme_preset.rfind("file:", 0) == 0;
    if (!known) theme_preset = "dark";
    for (auto& k : keybinds)
        if (k < 0 || (k & 0xFFFF) >= SDL_NUM_SCANCODES || k > (kBindCtrl | kBindShift | kBindAlt | 0xFFFF)) k = 0;
}

std::string prefsPath() { return joinPath(dataDir(), "ui_prefs.json"); }

namespace {

template <typename T>
void get(const json& j, const char* key, T& out) {
    if (!j.contains(key)) return;
    try {
        out = j.at(key).get<T>();
    } catch (const std::exception&) {
        // Wrong type: keep the default.
    }
}

} // namespace

UiPrefs loadPrefs() {
    UiPrefs p;
    bool ok = false;
    std::string text = readFile(prefsPath(), &ok);
    if (!ok) return p;
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return p;
    get(j, "theme_preset", p.theme_preset);
    get(j, "font_scale", p.font_scale);
    get(j, "custom_font_path", p.custom_font_path);
    get(j, "crisp_font", p.crisp_font);
    get(j, "reduce_motion", p.reduce_motion);
    get(j, "show_tooltips", p.show_tooltips);
    get(j, "toast_seconds", p.toast_seconds);
    get(j, "discovery_timeout_ms", p.discovery_timeout_ms);
    get(j, "left_width", p.left_width);
    get(j, "right_width", p.right_width);
    get(j, "left_collapsed", p.left_collapsed);
    get(j, "right_collapsed", p.right_collapsed);
    get(j, "device_fit", p.device_fit);
    if (j.contains("window") && j["window"].is_object()) {
        const json& w = j["window"];
        get(w, "x", p.window.x);
        get(w, "y", p.window.y);
        get(w, "w", p.window.w);
        get(w, "h", p.window.h);
        get(w, "maximized", p.window.maximized);
    }
    if (j.contains("keybinds") && j["keybinds"].is_object()) {
        const json& k = j["keybinds"];
        for (int i = 0; i < kActionCount; ++i) get(k, actionId(static_cast<Action>(i)), p.keybinds[static_cast<size_t>(i)]);
    }
    p.clamp();
    return p;
}

bool savePrefs(const UiPrefs& p) {
    json j;
    j["theme_preset"] = p.theme_preset;
    j["font_scale"] = p.font_scale;
    j["custom_font_path"] = p.custom_font_path;
    j["crisp_font"] = p.crisp_font;
    j["reduce_motion"] = p.reduce_motion;
    j["show_tooltips"] = p.show_tooltips;
    j["toast_seconds"] = p.toast_seconds;
    j["discovery_timeout_ms"] = p.discovery_timeout_ms;
    j["left_width"] = p.left_width;
    j["right_width"] = p.right_width;
    j["left_collapsed"] = p.left_collapsed;
    j["right_collapsed"] = p.right_collapsed;
    j["device_fit"] = p.device_fit;
    j["window"] = {{"x", p.window.x}, {"y", p.window.y}, {"w", p.window.w}, {"h", p.window.h},
                   {"maximized", p.window.maximized}};
    json& k = j["keybinds"];
    for (int i = 0; i < kActionCount; ++i) k[actionId(static_cast<Action>(i))] = p.keybinds[static_cast<size_t>(i)];
    return writeFile(prefsPath(), j.dump(2));
}

} // namespace ks::app
