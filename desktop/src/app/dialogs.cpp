// Native file dialogs and "open folder".
// Windows: GetSaveFileNameW, GetOpenFileNameW, SHBrowseForFolderW, ShellExecuteW.
// Other platforms: the export goes to settings.lastExportDir (or
// <data>/exports). The UI shows the path.
#include "app/app.h"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#endif

namespace ks::app {

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A double-NUL list for OPENFILENAMEW.lpstrFilter.
std::wstring filterFor(OutputFormat f) {
    std::wstring name = widen(formatLabel(f));
    std::wstring ext = widen(extensionFor(f));
    std::wstring s = name + L" (*." + ext + L")";
    s.push_back(L'\0');
    s += L"*." + ext;
    s.push_back(L'\0');
    s += L"All files (*.*)";
    s.push_back(L'\0');
    s += L"*.*";
    s.push_back(L'\0');
    return s;
}

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

} // namespace

std::optional<std::string> saveFileDialog(App& app, const std::string& defaultName, OutputFormat f) {
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring def = widen(defaultName);
    if (def.size() < MAX_PATH * 4 - 1) std::copy(def.begin(), def.end(), buf);
    std::wstring filter = filterFor(f);
    std::wstring ext = widen(extensionFor(f));
    std::wstring init = widen(app.settings.lastExportDir);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrDefExt = ext.c_str();
    ofn.lpstrInitialDir = init.empty() ? nullptr : init.c_str();
    ofn.lpstrTitle = L"Export";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    std::string path = narrow(buf);
    std::string dir = dirOf(path);
    if (!dir.empty() && dir != app.settings.lastExportDir) {
        app.settings.lastExportDir = dir;
        app.markSettingsChanged();
    }
    return path;
}

std::optional<std::string> openFileDialog() {
    wchar_t buf[MAX_PATH * 4] = {};
    static const wchar_t filter[] =
        L"Articles (*.html;*.htm;*.md;*.txt)\0*.html;*.htm;*.xhtml;*.md;*.markdown;*.txt\0All files (*.*)\0*.*\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = L"Open article";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return narrow(buf);
}

namespace {

std::wstring genericFilter(const std::string& desc, const std::string& pattern) {
    std::wstring s = widen(desc + " (" + pattern + ")");
    s.push_back(L'\0');
    s += widen(pattern);
    s.push_back(L'\0');
    s += L"All files (*.*)";
    s.push_back(L'\0');
    s += L"*.*";
    s.push_back(L'\0');
    return s;
}

} // namespace

std::optional<std::string> openFileDialogFiltered(const std::string& title, const std::string& filterDesc,
                                                  const std::string& pattern) {
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring filter = genericFilter(filterDesc, pattern);
    std::wstring t = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = t.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return narrow(buf);
}

std::optional<std::string> saveFileDialogFiltered(const std::string& title, const std::string& filterDesc,
                                                  const std::string& pattern, const std::string& defaultPath) {
    wchar_t buf[MAX_PATH * 4] = {};
    std::wstring def = widen(defaultPath);
    for (auto& c : def)
        if (c == L'/') c = L'\\';
    if (def.size() < MAX_PATH * 4 - 1) std::copy(def.begin(), def.end(), buf);
    std::wstring filter = genericFilter(filterDesc, pattern);
    std::wstring t = widen(title);
    std::wstring ext;
    size_t dot = pattern.find_last_of('.');
    if (dot != std::string::npos) ext = widen(pattern.substr(dot + 1));
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
    ofn.lpstrTitle = t.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return narrow(buf);
}

std::optional<std::string> pickFolderDialog(const std::string& title) {
    std::wstring t = widen(title);
    BROWSEINFOW bi{};
    bi.lpszTitle = t.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return std::nullopt;
    wchar_t buf[MAX_PATH] = {};
    bool ok = SHGetPathFromIDListW(pidl, buf) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return std::nullopt;
    return narrow(buf);
}

void openFolder(const std::string& path) {
    ShellExecuteW(nullptr, L"open", widen(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

#else

std::optional<std::string> saveFileDialog(App& app, const std::string& defaultName, OutputFormat) {
    std::string dir = app.settings.lastExportDir;
    if (dir.empty()) {
        dir = joinPath(dataDir(), "exports");
        app.settings.lastExportDir = dir;
        app.markSettingsChanged();
    }
    makeDirs(dir);
    std::string path = joinPath(dir, defaultName);
    app.statusMsg = "Export goes to " + path;
    app.addLog("No native save dialog here. Export path: " + path);
    return path;
}

std::optional<std::string> openFileDialog() { return std::nullopt; }

std::optional<std::string> pickFolderDialog(const std::string&) { return std::nullopt; }

std::optional<std::string> openFileDialogFiltered(const std::string&, const std::string&, const std::string&) {
    return std::nullopt;
}

std::optional<std::string> saveFileDialogFiltered(const std::string&, const std::string&, const std::string&,
                                                  const std::string& defaultPath) {
    return defaultPath;
}

void openFolder(const std::string& path) {
    std::string url = "file://" + path;
    SDL_OpenURL(url.c_str());
}

#endif

// The old UI scale file <data>/ui_scale.txt. ui_prefs.json replaces it.
float loadUiScale() {
    bool ok = false;
    std::string s = readFile(joinPath(dataDir(), "ui_scale.txt"), &ok);
    if (!ok) return 1.0f;
    float v = static_cast<float>(std::atof(s.c_str()));
    return (v >= 0.75f && v <= 2.5f) ? v : 1.0f;
}

} // namespace ks::app
