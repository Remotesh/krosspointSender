// Window chrome: menu bar, main window, status bar, toasts.
// The three columns are in layout.cpp.
// Ported from weatherDesktop (MIT, Cody Savage): the status bar layout,
// the right-aligned button groups and the slide-in toast overlay.
#include "app/ui.h"

#include <SDL.h>

#include <array>
#include <cstdio>
#include <ctime>

namespace ks::app {

namespace {

// Shortcut labels for the menu. Rebuilt only when a bind changes.
const char* shortcutLabel(const App& app, Action a) {
    static std::array<int, kActionCount> last{};
    static std::array<std::string, kActionCount> text;
    size_t i = static_cast<size_t>(a);
    if (last[i] != app.prefs.keybinds[i] || text[i].empty()) {
        last[i] = app.prefs.keybinds[i];
        text[i] = app.prefs.keybinds[i] ? formatKeybind(app.prefs.keybinds[i]) : std::string(" ");
    }
    return text[i].c_str();
}

// "HH:MM", refreshed once a minute.
const char* clockText() {
    static char buf[8] = "--:--";
    static std::time_t lastMin = -1;
    std::time_t t = std::time(nullptr);
    if (t / 60 != lastMin) {
        lastMin = t / 60;
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::strftime(buf, sizeof buf, "%H:%M", &tmv);
    }
    return buf;
}

void drawStatusBar(App& app, float height) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - height));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, height));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav;
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(st.ItemSpacing.x * 1.5f, st.FramePadding.y * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##status", nullptr, f);
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(wp, ImVec2(wp.x + ImGui::GetWindowWidth(), wp.y),
                                        ImGui::GetColorU32(theme::Muted));
    ImGui::SetCursorPosY((height - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::AlignTextToFramePadding();

    // Left: helper server state, then the job state.
    bool on = app.helper.running();
    bool failed = !on && app.settings.helperServerEnabled;
    ImGui::TextColored(on ? theme::Live : (failed ? theme::Error : theme::Dim), "%s", app.helperStatus.c_str());
    int n = 0;
    for (auto& j : app.jobs)
        if (!j->done && j->kind != JobKind::Quiet) ++n;
    ImGui::SameLine();
    ImGui::TextDisabled(" - ");
    ImGui::SameLine();
    if (n) {
        ImGui::TextColored(theme::Amber, "working %c  %d job%s", ui::SpinnerChar(), n, n == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::SmallButton("cancel all")) app.cancelAll();
        ui::Tip("cancel every running job");
    } else {
        ImGui::TextDisabled("idle");
    }

    // Right: images, clock.
    char img[48] = "";
    if (app.article && !app.article->images.empty())
        std::snprintf(img, sizeof img, "img %d/%d", static_cast<int>(app.images.size()),
                      static_cast<int>(app.article->images.size()));
    const char* clock = clockText();
    float rightW = ImGui::CalcTextSize(clock).x + (img[0] ? ImGui::CalcTextSize(img).x + st.ItemSpacing.x * 2 : 0);

    // Middle: the last log line, clipped before the right group.
    ImGui::SameLine(0, st.ItemSpacing.x * 2);
    float x0 = ImGui::GetCursorScreenPos().x;
    float x1 = wp.x + ImGui::GetWindowWidth() - st.WindowPadding.x - rightW - st.ItemSpacing.x * 2;
    if (!app.log.empty() && x1 > x0 + 40.f) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(ImVec2(x0, wp.y), ImVec2(x1, wp.y + height), true);
        ImGui::TextDisabled("%s  %s", app.log.back().time.c_str(), app.log.back().text.c_str());
        ImGui::PopClipRect();
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(x1, p.y));
    }
    ui::RightAlign(rightW);
    if (img[0]) {
        bool all = app.images.size() >= app.article->images.size();
        ImGui::TextColored(all ? theme::Dim : theme::Amber, "%s", img);
        ui::Tip("images loaded for this article");
        ImGui::SameLine(0, st.ItemSpacing.x * 2);
    }
    ImGui::TextUnformatted(clock);
    ImGui::End();
    ImGui::PopStyleVar(3);
}

ImVec4 toastColor(ToastKind k) {
    switch (k) {
        case ToastKind::Warn: return theme::Warn;
        case ToastKind::Error: return theme::Error;
        default: return theme::Info;
    }
}

const char* toastTag(ToastKind k) {
    switch (k) {
        case ToastKind::Warn: return "warn";
        case ToastKind::Error: return "error";
        default: return "info";
    }
}

constexpr double kSlideSeconds = 0.28;
constexpr double kFadeSeconds = 0.4;

double toastLife(const App& app, const Toast& t) {
    return app.prefs.toast_seconds * (t.kind == ToastKind::Error ? 2.0 : 1.0);
}

} // namespace

bool toastsAnimating(const App& app) {
    double now = appClock();
    for (auto& t : app.toasts) {
        double age = now - t.born;
        if (age < kSlideSeconds || age > toastLife(app, t) - kFadeSeconds) return true;
    }
    return false;
}

void drawToasts(App& app) {
    if (app.toasts.empty()) return;
    double now = appClock();
    // Drop the old ones first.
    std::vector<Toast> keep;
    for (auto& t : app.toasts)
        if (now - t.born < toastLife(app, t)) keep.push_back(t);
    app.toasts.swap(keep);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float w = ImGui::GetFontSize() * 22.f;
    float y = vp->WorkPos.y + ImGui::GetFrameHeight() * 0.5f;
    int removeId = 0;
    for (auto& t : app.toasts) {
        double age = now - t.born;
        float slide = 0.f;
        if (!ui::g_reduceMotion && age < kSlideSeconds) {
            float p = static_cast<float>(age / kSlideSeconds);
            float e = 1.0f - (1.0f - p) * (1.0f - p);   // ease-out
            slide = (1.0f - e) * (w + 24.0f);
        }
        float alpha = 1.f;
        double left = toastLife(app, t) - age;
        if (left < kFadeSeconds) alpha = static_cast<float>(left / kFadeSeconds);
        ImVec4 kc = toastColor(t.kind);
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - w - 16.f + slide, y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(w, 0), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.97f);
        ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::Panel);
        ImGui::PushStyleColor(ImGuiCol_Border, kc);
        char id[32];
        std::snprintf(id, sizeof id, "##toast%d", t.id);
        ImGui::Begin(id, nullptr, f);
        float xBtn = ImGui::GetFrameHeight();
        ImGui::TextColored(kc, "%s", toastTag(t.kind));
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - xBtn - ImGui::GetStyle().ItemSpacing.x);
        ImGui::TextWrapped("%s", t.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - xBtn);
        if (ImGui::SmallButton("x")) removeId = t.id;
        ui::Tip("dismiss");
        // Keep a toast while the pointer rests on it.
        if (ImGui::IsWindowHovered() && age > kSlideSeconds) t.born = now - kSlideSeconds;
        y += ImGui::GetWindowHeight() + 6.f;
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
    if (removeId) {
        for (size_t i = 0; i < app.toasts.size(); ++i)
            if (app.toasts[i].id == removeId) app.toasts.erase(app.toasts.begin() + static_cast<long>(i));
    }
}

void drawMenuBar(App& app) {
    if (!ImGui::BeginMainMenuBar()) return;
    ImGui::TextColored(theme::Amber, "k-rosspointSender");
    ImGui::TextDisabled("|");
    if (ImGui::BeginMenu("file")) {
        if (ImGui::MenuItem("paste", shortcutLabel(app, Action::Paste))) pasteFromClipboard(app);
        if (ImGui::MenuItem("convert", shortcutLabel(app, Action::Convert))) convertCurrent(app);
        if (ImGui::MenuItem("export...", shortcutLabel(app, Action::Export), false, app.article.has_value()))
            exportNow(app);
        ImGui::Separator();
        bool quit = false;
        if (ImGui::MenuItem("quit", shortcutLabel(app, Action::Quit))) quit = true;
        if (quit) {
            SDL_Event ev{};
            ev.type = SDL_QUIT;
            SDL_PushEvent(&ev);
        }
        ImGui::EndMenu();
    }
    ImGui::MenuItem("library", shortcutLabel(app, Action::OpenLibrary), &app.showLibrary);
    bool devOpen = app.showDevices;
    if (ImGui::MenuItem("devices", shortcutLabel(app, Action::OpenDevices), &devOpen)) {
        if (devOpen) openDevicesWindow(app);
        else app.showDevices = false;
    }
    if (ImGui::BeginMenu("view")) {
        bool srcOpen = !app.prefs.left_collapsed, rightOpen = !app.prefs.right_collapsed;
        if (ImGui::MenuItem("source column", shortcutLabel(app, Action::ToggleLeftPanel), &srcOpen)) {
            app.prefs.left_collapsed = !srcOpen;
            app.markPrefsChanged();
        }
        if (ImGui::MenuItem("right column", shortcutLabel(app, Action::ToggleRightPanel), &rightOpen)) {
            app.prefs.right_collapsed = !rightOpen;
            app.markPrefsChanged();
        }
        ImGui::MenuItem("device preview", shortcutLabel(app, Action::ToggleDevicePreview), &app.devicePreview);
        ImGui::Separator();
        if (ImGui::MenuItem("reset column widths")) {
            app.prefs.left_width = 380.f;
            app.prefs.right_width = 0.f;
            app.prefs.left_collapsed = app.prefs.right_collapsed = false;
            app.markPrefsChanged();
        }
        ImGui::EndMenu();
    }
    ImGui::MenuItem("log", shortcutLabel(app, Action::OpenLog), &app.showLog);
    ImGui::MenuItem("settings", shortcutLabel(app, Action::OpenSettings), &app.showSettings);
    drawDeviceBar(app);
    ImGui::EndMainMenuBar();
}

void drawMainLayout(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle& st = ImGui::GetStyle();
    float statusH = ImGui::GetFrameHeight() + st.FramePadding.y * 2.f;
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusH));
    ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(st.ItemSpacing.x, st.ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::Begin("##main", nullptr, f);
    ImGui::PopStyleVar(3);
    drawColumns(app, ImGui::GetContentRegionAvail().y);
    ImGui::End();
    drawStatusBar(app, statusH);
}

} // namespace ks::app
