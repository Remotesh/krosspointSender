// Keyboard shortcuts: actions, packed binds, capture widget.
// Ported from the k-os client (same author), relicensed MIT:
// format_keybind, matches_keybind and the shortcut capture widget.
// A packed bind is scancode | ctrl << 16 | shift << 17 | alt << 18.
#pragma once

#include <SDL.h>

#include <string>

namespace ks::app {

struct App;

enum class Action {
    Convert,
    Fetch,
    Paste,
    Export,
    Send,
    SendFile,
    Discover,
    OpenLibrary,
    OpenSettings,
    OpenDevices,
    OpenLog,
    ToggleDevicePreview,
    ToggleLeftPanel,
    ToggleRightPanel,
    CancelJobs,
    Quit,
    Count
};
constexpr int kActionCount = static_cast<int>(Action::Count);

constexpr int kBindCtrl = 1 << 16;
constexpr int kBindShift = 1 << 17;
constexpr int kBindAlt = 1 << 18;

const char* actionId(Action a);      // JSON key, e.g. "open_settings"
const char* actionLabel(Action a);   // UI label
int defaultBind(Action a);
std::string formatKeybind(int packed);
bool matchesKeybind(const SDL_KeyboardEvent& key, int packed);

// Run the action of a key press. Returns true when a bind matched.
// textActive is true while an ImGui text box has focus. Then only the
// binds with Ctrl or Alt that do not clash with text editing run.
bool dispatchKey(App& app, const SDL_KeyboardEvent& key, bool textActive, bool& quit);
void runAction(App& app, Action a, bool& quit);

// The capture button for one bind. Returns true when the bind changed.
bool shortcutKeybindWidget(const char* id, int& packed, float width);
bool captureActive();   // true while the capture widget waits for a key

} // namespace ks::app
