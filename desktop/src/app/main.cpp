// K-rosspointSender desktop: entry point. SDL2 + OpenGL3 + Dear ImGui boot,
// the lazy event loop, shortcuts, drop events, and the CLI switch.
// Ported from weatherDesktop (MIT, Cody Savage): the lazy render loop,
// the DPI hint and the debounced window geometry save.
#include "app/ui.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace ks;
using namespace ks::app;

namespace {

// The WIN32 subsystem has no console. Attach to the parent console when
// the user gives arguments. Keep stdout when it is already a pipe or file.
void attachConsole() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD type = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE || type == FILE_TYPE_CHAR) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        f = std::freopen("CONOUT$", "w", stdout);
        f = std::freopen("CONOUT$", "w", stderr);
        (void)f;
        std::printf("\n");
    }
#endif
}

void boot(App& app) {
    try {
        app.settings = net::loadSettings();
    } catch (const std::exception& e) {
        app.addLog(std::string("Cannot read settings, defaults used: ") + e.what());
    }
    bool hadPrefs = false;
    readFile(prefsPath(), &hadPrefs);
    app.prefs = loadPrefs();
    if (!hadPrefs) app.prefs.font_scale = loadUiScale();   // migrate ui_scale.txt
    app.applyPreset(app.prefs.theme_preset);
    app.prefsDirty = !hadPrefs;
    app.reloadProfiles();
    app.addLog("Data folder: " + dataDir());
    app.restartHelper();
    if (app.profiles.empty()) app.addLog("No device yet. Use add device in the menu bar.");
    app.addLog("UI prefs: " + prefsPath() + " (theme " + app.prefs.theme_preset + ").");
}

void shutdown(App& app) {
    app.helperDirty = false;   // no restart on the way out
    app.flushSaves();
    app.helper.stop();
    app.cancelAll();
    // Give the jobs a short time to stop. Detached threads end with the process.
    double t0 = appClock();
    while (!app.jobs.empty() && appClock() - t0 < 1.5) {
        app.drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// Pick a saved window position only when it lies on a display.
bool onSomeDisplay(int x, int y, int w, int h) {
    int n = SDL_GetNumVideoDisplays();
    SDL_Point c{x + w / 2, y + h / 2};
    for (int i = 0; i < n; ++i) {
        SDL_Rect r;
        if (SDL_GetDisplayUsableBounds(i, &r) == 0 && SDL_PointInRect(&c, &r)) return true;
    }
    return false;
}

void captureGeometry(App& app, SDL_Window* win) {
    Uint32 fl = SDL_GetWindowFlags(win);
    if (fl & SDL_WINDOW_MINIMIZED) return;
    WindowGeom& g = app.prefs.window;
    if (fl & SDL_WINDOW_MAXIMIZED) {
        g.maximized = true;   // keep the restored extent from before
    } else {
        g.maximized = false;
        SDL_GetWindowPosition(win, &g.x, &g.y);
        SDL_GetWindowSize(win, &g.w, &g.h);
    }
    app.markPrefsChanged();
    app.prefsChangedAt -= 1.0;   // the geometry debounce already waited
}

// Lazy rendering (weatherDesktop App::frameTimeoutMs): block until input or
// the next repaint is due. Any event, also a requestFrame() from a worker,
// wakes the loop at once.
int frameTimeoutMs(const App& app, SDL_Window* win, double sinceInput) {
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) return 200;
    if (sinceInput < 0.5 || captureActive() || toastsAnimating(app)) return 16;
    if (!app.jobs.empty() || !app.toasts.empty()) return 100;   // spinner, toast timers
    return 250;
}

int runGui() {
    // Per-monitor DPI awareness. Set it before SDL_Init.
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    initWakeEvent();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");

    App app;
    boot(app);
    WindowGeom g = app.prefs.window;
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED;
    if (g.x != -1 && g.y != -1 && onSomeDisplay(g.x, g.y, g.w, g.h)) {
        x = g.x;
        y = g.y;
    }
    auto wflags = static_cast<SDL_WindowFlags>(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* win = SDL_CreateWindow("K-rosspointSender", x, y, g.w, g.h, wflags);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (g.maximized) SDL_MaximizeWindow(win);
    SDL_GLContext gl = SDL_GL_CreateContext(win);
    if (!gl) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(win, gl);
    SDL_GL_SetSwapInterval(1);   // vsync
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_EventState(SDL_DROPTEXT, SDL_ENABLE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    static std::string iniPath = joinPath(dataDir(), "imgui.ini");
    io.IniFilename = iniPath.c_str();
    ImGui_ImplSDL2_InitForOpenGL(win, gl);
    ImGui_ImplOpenGL3_Init("#version 150");
    rebuildFonts(app);
    applyStyle(app);

    bool running = true;
    bool testBusy = false;   // the UI test hook has queued input
    double lastInput = appClock();
    bool geomDirty = false;
    double geomAt = 0;
    auto handle = [&](SDL_Event& ev) {
        ImGui_ImplSDL2_ProcessEvent(&ev);
        switch (ev.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN: {
                bool quit = false;
                dispatchKey(app, ev.key, io.WantTextInput, quit);
                if (quit) running = false;
                break;
            }
            case SDL_WINDOWEVENT:
                if (ev.window.windowID != SDL_GetWindowID(win)) break;
                switch (ev.window.event) {
                    case SDL_WINDOWEVENT_CLOSE: running = false; break;
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_MOVED:
                    case SDL_WINDOWEVENT_MAXIMIZED:
                    case SDL_WINDOWEVENT_RESTORED:
                        geomDirty = true;
                        geomAt = appClock();
                        break;
                    case SDL_WINDOWEVENT_DISPLAY_CHANGED: app.styleDirty = true; break;
                    default: break;
                }
                break;
            case SDL_DROPFILE:
                if (ev.drop.file) {
                    handleDropFile(app, ev.drop.file);
                    SDL_free(ev.drop.file);
                }
                break;
            case SDL_DROPTEXT:
                if (ev.drop.file) {
                    handleDropText(app, ev.drop.file);
                    SDL_free(ev.drop.file);
                }
                break;
            default: break;
        }
    };

    while (running) {
        SDL_Event ev;
        int waitMs = testBusy ? 16 : frameTimeoutMs(app, win, appClock() - lastInput);
        if (SDL_WaitEventTimeout(&ev, waitMs)) {
            lastInput = appClock();
            handle(ev);
            while (SDL_PollEvent(&ev)) handle(ev);
        }
        double now = appClock();
        app.drain();
        app.tick(now);
        // Save the geometry 700 ms after the last move or resize.
        if (geomDirty && now - geomAt >= 0.7) {
            geomDirty = false;
            captureGeometry(app, win);
        }
        if (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED) continue;   // nothing to draw
        if (app.fontsDirty) rebuildFonts(app);
        if (app.styleDirty) applyStyle(app);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        testBusy = uiTestPoll(app);
        ImGui::NewFrame();
        drawMenuBar(app);
        drawMainLayout(app);
        drawLibraryWindow(app);
        drawSettingsWindow(app);
        drawDevicesWindow(app);
        drawLogWindow(app);
        drawToasts(app);
        ImGui::Render();
        glViewport(0, 0, static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
                   static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y));
        glClearColor(theme::Bg.x, theme::Bg.y, theme::Bg.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        uiTestAfterRender(static_cast<int>(io.DisplaySize.x * io.DisplayFramebufferScale.x),
                          static_cast<int>(io.DisplaySize.y * io.DisplayFramebufferScale.y));
        SDL_GL_SwapWindow(win);
    }

    if (geomDirty) captureGeometry(app, win);
    shutdown(app);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (isCliArgs(argc, argv)) {
        attachConsole();
        int rc = runCli(argc, argv);
        std::fflush(stdout);
        std::fflush(stderr);
        return rc;
    }
    try {
        return runGui();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }
}
