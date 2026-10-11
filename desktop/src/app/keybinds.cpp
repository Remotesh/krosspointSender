// Keyboard shortcuts: defaults, format, match, dispatch, capture widget.
// Ported from the k-os client (same author), relicensed MIT:
// format_keybind, matches_keybind, render_shortcut_keybind.
#include "app/keybinds.h"

#include "app/ui.h"

#include <imgui.h>

namespace ks::app {

namespace {

struct ActionDef {
    const char* id;
    const char* label;
    int bind;
};

constexpr int C = kBindCtrl;
constexpr int S = kBindShift;

const ActionDef kDefs[kActionCount] = {
    {"convert", "convert the current source", C | SDL_SCANCODE_RETURN},
    {"fetch", "fetch the url box", C | SDL_SCANCODE_L},
    {"paste", "paste from the clipboard", C | SDL_SCANCODE_V},
    {"export", "export...", C | SDL_SCANCODE_E},
    {"send", "send to the device", C | S | SDL_SCANCODE_S},
    {"discover", "discover devices", C | SDL_SCANCODE_D},
    {"open_library", "library window", C | SDL_SCANCODE_B},
    {"open_settings", "settings window", C | SDL_SCANCODE_COMMA},
    {"open_devices", "devices window", C | S | SDL_SCANCODE_D},
    {"open_log", "log window", C | S | SDL_SCANCODE_L},
    {"toggle_device_preview", "device preview on/off", C | SDL_SCANCODE_P},
    {"toggle_left_panel", "source column open/closed", C | SDL_SCANCODE_LEFTBRACKET},
    {"toggle_right_panel", "right column open/closed", C | SDL_SCANCODE_RIGHTBRACKET},
    {"cancel_jobs", "cancel running jobs", SDL_SCANCODE_ESCAPE},
    {"quit", "quit", C | SDL_SCANCODE_Q},
};

// The capture widget state. Only one bind captures at a time.
int* g_captureTarget = nullptr;

// Ctrl chords that a text box uses. They never run an action while a
// text box has focus.
bool isTextEditChord(int packed) {
    if (!(packed & kBindCtrl) || (packed & kBindAlt)) return false;
    switch (packed & 0xFFFF) {
        case SDL_SCANCODE_A: case SDL_SCANCODE_C: case SDL_SCANCODE_V: case SDL_SCANCODE_X:
        case SDL_SCANCODE_Y: case SDL_SCANCODE_Z: case SDL_SCANCODE_LEFT: case SDL_SCANCODE_RIGHT:
        case SDL_SCANCODE_UP: case SDL_SCANCODE_DOWN: case SDL_SCANCODE_HOME: case SDL_SCANCODE_END:
        case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_DELETE: case SDL_SCANCODE_INSERT:
            return true;
        default:
            return false;
    }
}

bool toggle(bool& b) {
    b = !b;
    return true;
}

} // namespace

const char* actionId(Action a) { return kDefs[static_cast<int>(a)].id; }
const char* actionLabel(Action a) { return kDefs[static_cast<int>(a)].label; }
int defaultBind(Action a) { return kDefs[static_cast<int>(a)].bind; }

std::string formatKeybind(int packed) {
    if (packed == 0) return "none";
    std::string r;
    if (packed & kBindCtrl) r += "Ctrl+";
    if (packed & kBindShift) r += "Shift+";
    if (packed & kBindAlt) r += "Alt+";
    const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(packed & 0xFFFF));
    r += (name && name[0]) ? name : "?";
    return r;
}

bool matchesKeybind(const SDL_KeyboardEvent& key, int packed) {
    if (packed == 0) return false;
    if (static_cast<int>(key.keysym.scancode) != (packed & 0xFFFF)) return false;
    Uint16 mods = key.keysym.mod;
    bool ctrl = (mods & KMOD_CTRL) != 0, shift = (mods & KMOD_SHIFT) != 0, alt = (mods & KMOD_ALT) != 0;
    return ctrl == ((packed & kBindCtrl) != 0) && shift == ((packed & kBindShift) != 0) &&
           alt == ((packed & kBindAlt) != 0);
}

void runAction(App& app, Action a, bool& quit) {
    switch (a) {
        case Action::Convert: convertCurrent(app); break;
        case Action::Fetch: fetchCurrent(app); break;
        case Action::Paste: pasteFromClipboard(app); break;
        case Action::Export:
            if (app.article && !app.busy(JobKind::Export)) exportNow(app);
            break;
        case Action::Send:
            if (!app.busy(JobKind::Send)) jobSend(app);
            if (!app.sendStatus.empty() && !app.busy(JobKind::Send)) app.toast(ToastKind::Warn, app.sendStatus);
            break;
        case Action::Discover:
            if (!app.busy(JobKind::Discover)) jobDiscover(app);
            break;
        case Action::OpenLibrary: toggle(app.showLibrary); break;
        case Action::OpenSettings: toggle(app.showSettings); break;
        case Action::OpenDevices:
            if (app.showDevices) app.showDevices = false;
            else openDevicesWindow(app);
            break;
        case Action::OpenLog: toggle(app.showLog); break;
        case Action::ToggleDevicePreview: toggle(app.devicePreview); break;
        case Action::ToggleLeftPanel:
            toggle(app.prefs.left_collapsed);
            app.markPrefsChanged();
            break;
        case Action::ToggleRightPanel:
            toggle(app.prefs.right_collapsed);
            app.markPrefsChanged();
            break;
        case Action::CancelJobs:
            if (!app.jobs.empty()) {
                app.cancelAll();
                app.toast(ToastKind::Warn, "cancel requested for running jobs");
            }
            break;
        case Action::Quit: quit = true; break;
        case Action::Count: break;
    }
}

bool dispatchKey(App& app, const SDL_KeyboardEvent& key, bool textActive, bool& quit) {
    if (key.repeat || g_captureTarget) return false;
    for (int i = 0; i < kActionCount; ++i) {
        int packed = app.prefs.keybinds[static_cast<size_t>(i)];
        if (!matchesKeybind(key, packed)) continue;
        if (textActive && (!(packed & (kBindCtrl | kBindAlt)) || isTextEditChord(packed))) return false;
        auto a = static_cast<Action>(i);
        // Escape also closes popups. Leave it to ImGui while one is open.
        if (a == Action::CancelJobs && ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) return false;
        runAction(app, a, quit);
        return true;
    }
    return false;
}

bool captureActive() { return g_captureTarget != nullptr; }

bool shortcutKeybindWidget(const char* id, int& packed, float width) {
    bool changed = false;
    ImGui::PushID(id);
    if (g_captureTarget == &packed) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Amber);
        ImGui::Button("press a shortcut...", ImVec2(width, 0));
        ImGui::PopStyleColor();
        // Poll the keyboard for a key that is not a modifier.
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        for (int i = 4; i < SDL_NUM_SCANCODES; ++i) {
            if (i >= SDL_SCANCODE_LCTRL && i <= SDL_SCANCODE_RGUI) continue;
            if (!keys[i]) continue;
            if (i != SDL_SCANCODE_ESCAPE) {
                SDL_Keymod mods = SDL_GetModState();
                int p = i;
                if (mods & KMOD_CTRL) p |= kBindCtrl;
                if (mods & KMOD_SHIFT) p |= kBindShift;
                if (mods & KMOD_ALT) p |= kBindAlt;
                changed = p != packed;
                packed = p;
            }
            g_captureTarget = nullptr;   // Escape cancels
            break;
        }
        // A click elsewhere also cancels.
        if (g_captureTarget && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered())
            g_captureTarget = nullptr;
    } else {
        std::string label = formatKeybind(packed);
        if (ImGui::Button(label.c_str(), ImVec2(width, 0))) g_captureTarget = &packed;
        ui::Tip("click, then press the new shortcut. escape cancels.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(packed == 0);
    if (ImGui::SmallButton("x")) {
        packed = 0;
        changed = true;
    }
    ImGui::EndDisabled();
    if (ui::g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("clear this shortcut");
    ImGui::PopID();
    return changed;
}

} // namespace ks::app
