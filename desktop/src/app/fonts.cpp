// Display scale, fonts, style apply and the Windows 11 title bar tint.
// Ported from weatherDesktop (MIT, Cody Savage): displayScale(),
// rebuildFonts() (Cascadia Mono, then Consolas, else the default font) and
// the DWM caption colors.
#include "app/ui.h"

#include <SDL.h>
#include <SDL_syswm.h>

#include <cmath>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ks::app {

namespace {

constexpr float kBaseFontPx = 16.0f;

bool fileExists(const std::string& path) {
    std::ifstream probe(path, std::ios::binary);
    return probe.good();
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

float displayScale() {
    SDL_Window* win = SDL_GL_GetCurrentWindow();
    int idx = win ? SDL_GetWindowDisplayIndex(win) : 0;
    if (idx < 0) idx = 0;
    float ddpi = 96.0f;
    if (SDL_GetDisplayDPI(idx, &ddpi, nullptr, nullptr) != 0 || ddpi <= 0.0f) return 1.0f;
    return ddpi / 96.0f;
}

void rebuildFonts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    app.fontsCrisp = false;
    app.fontName.clear();
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    // A custom font wins when the file is there.
    if (!app.prefs.custom_font_path.empty() && fileExists(app.prefs.custom_font_path) &&
        io.Fonts->AddFontFromFileTTF(app.prefs.custom_font_path.c_str(), kBaseFontPx, &cfg)) {
        app.fontsCrisp = true;
        app.fontName = baseName(app.prefs.custom_font_path);
    }
    if (!app.fontsCrisp && app.prefs.crisp_font) {
        // A real monospace TTF at its display size is far sharper than the
        // stretched bitmap font. Both ship with Windows.
        const char* candidates[] = {"C:\\Windows\\Fonts\\CascadiaMono.ttf", "C:\\Windows\\Fonts\\consola.ttf",
                                    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                                    "/System/Library/Fonts/Menlo.ttc"};
        for (const char* path : candidates) {
            if (!fileExists(path)) continue;
            if (io.Fonts->AddFontFromFileTTF(path, kBaseFontPx, &cfg)) {
                app.fontsCrisp = true;
                app.fontName = baseName(path);
                break;
            }
        }
    }
    if (app.fontsCrisp) {
        // Fallback glyphs that the monospace fonts do not have, for example
        // the arrow after an external link in a pack page (U+2197).
        const char* symbols[] = {"C:/Windows/Fonts/seguisym.ttf",
                                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
        for (const char* path : symbols) {
            if (!fileExists(path)) continue;
            ImFontConfig merge = cfg;
            merge.MergeMode = true;
            if (io.Fonts->AddFontFromFileTTF(path, kBaseFontPx, &merge)) break;
        }
    }
    if (!app.fontsCrisp) {
        ImFontConfig def;
        def.SizePixels = kBaseFontPx;
        io.Fonts->AddFontDefault(&def);
        app.fontName = "built-in";
    }
    // ImGui 1.92+: the backend picks up the new atlas on the next frame.
    app.fontsDirty = false;
}

void applyStyle(App& app) {
    float dpi = displayScale();
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontSizeBase = kBaseFontPx;
    st.FontScaleMain = app.prefs.font_scale;
    st.FontScaleDpi = dpi;
    // theme.apply() resets the sizes first, so this stays idempotent.
    app.theme.apply(dpi * app.prefs.font_scale);
    ui::g_showTooltips = app.prefs.show_tooltips;
    ui::g_reduceMotion = app.prefs.reduce_motion;
    tintTitleBar(SDL_GL_GetCurrentWindow(), app.theme);
    app.styleDirty = false;
}

#ifdef _WIN32

namespace {

// dwmapi is not in the link line. Load it at run time.
using DwmSetFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

DwmSetFn dwmSet() {
    static DwmSetFn fn = []() -> DwmSetFn {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        return m ? reinterpret_cast<DwmSetFn>(reinterpret_cast<void*>(GetProcAddress(m, "DwmSetWindowAttribute")))
                 : nullptr;
    }();
    return fn;
}

COLORREF toRef(const ImVec4& c) {
    auto b = [](float v) { return static_cast<BYTE>(std::lround(std::fmin(std::fmax(v, 0.f), 1.f) * 255.f)); };
    return RGB(b(c.x), b(c.y), b(c.z));
}

} // namespace

void tintTitleBar(::SDL_Window* win, const Theme& t) {
    DwmSetFn set = dwmSet();
    if (!win || !set) return;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info)) return;
    HWND hwnd = info.info.win.window;
    // DWMWA_USE_IMMERSIVE_DARK_MODE 20, BORDER_COLOR 34, CAPTION_COLOR 35,
    // TEXT_COLOR 36. The colors apply on Windows 11 22000+ only; older
    // builds keep the dark or light immersive bar.
    BOOL dark = t.is_light() ? FALSE : TRUE;
    set(hwnd, 20, &dark, sizeof dark);
    COLORREF caption = toRef(t.bg), text = toRef(t.fg), border = toRef(t.muted);
    set(hwnd, 35, &caption, sizeof caption);
    set(hwnd, 36, &text, sizeof text);
    set(hwnd, 34, &border, sizeof border);
    // Windows 10 repaints the caption only on a frame change. Ask for one.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

#else

void tintTitleBar(::SDL_Window*, const Theme&) {}

#endif

} // namespace ks::app
