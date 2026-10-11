// Settings window: search filter, sections, fixed footer.
// Sections: Appearance, Keyboard Shortcuts (settings_appearance.cpp),
// Conversion, Output & Files, Spider packs (defaults), Device & Network,
// System, About.
// Every change applies at once. App settings save through
// net::saveSettings, UI prefs through savePrefs, both after a short delay.
// Ported from the k-os client (same author), relicensed MIT: the search
// box, filt_header and the fixed footer. The per-section reset is new.
#include "app/settings_ui.h"

#include <sstream>

namespace ks::app {

namespace settings {

std::string& filterText() {
    static std::string s;
    return s;
}

bool filterMatches(const char* text) {
    const std::string& f = filterText();
    if (trim(f).empty()) return true;
    return toLower(text).find(toLower(trim(f))) != std::string::npos;
}

bool header(const char* label, const char* keywords, ImGuiTreeNodeFlags flags) {
    if (trim(filterText()).empty()) return ImGui::CollapsingHeader(label, flags);
    if (!filterMatches(label) && (!keywords || !filterMatches(keywords))) return false;
    ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    return ImGui::CollapsingHeader(label, flags);
}

bool resetButton(const char* id) {
    static std::string armed;
    ImGui::PushID(id);
    bool confirm = armed == id;
    const char* label = confirm ? "click again to reset" : "reset to defaults";
    ui::RightAlign(ui::ButtonWidth(label, true), false);
    if (confirm) ImGui::PushStyleColor(ImGuiCol_Text, theme::Error);
    bool pressed = ImGui::SmallButton(label);
    if (confirm) ImGui::PopStyleColor();
    if (!confirm) ui::Tip("reset this section to the defaults");
    bool done = false;
    if (pressed) {
        if (confirm) {
            armed.clear();
            done = true;
        } else {
            armed = id;
        }
    } else if (confirm && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered()) {
        armed.clear();
    }
    ImGui::PopID();
    return done;
}

} // namespace settings

namespace {

using settings::header;
using settings::resetButton;

std::string joinLines(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += x + "\n";
    return s;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

bool drawStyle(App& app) {
    StyleSettings& y = app.settings.style;
    bool ch = false;
    StylePreset p = y.preset;
    if (ui::EnumCombo("style preset", p, ui::presetItems())) {
        std::string css = y.customCss;
        y = output::preset(p);
        y.preset = p;
        y.customCss = css;
        ch = true;
    }
    ui::Hint("A preset sets the numbers below. Change them after you pick one.");
    ch |= ImGui::SliderInt("font size %", &y.fontSizePercent, 70, 200);
    float f = static_cast<float>(y.marginEm);
    if (ImGui::SliderFloat("margin (em)", &f, 0.f, 3.f, "%.2f")) { y.marginEm = f; ch = true; }
    f = static_cast<float>(y.lineHeight);
    if (ImGui::SliderFloat("line height", &f, 1.f, 2.2f, "%.2f")) { y.lineHeight = f; ch = true; }
    f = static_cast<float>(y.paragraphSpacingEm);
    if (ImGui::SliderFloat("paragraph spacing (em)", &f, 0.f, 2.f, "%.2f")) { y.paragraphSpacingEm = f; ch = true; }
    ch |= ImGui::SliderInt("image max width (px)", &y.imageMaxWidthPx, 120, 1200);
    ch |= ImGui::SliderInt("jpeg quality", &y.imageQuality, 10, 100);
    ch |= ImGui::Checkbox("grayscale images", &y.grayscaleImages);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("keep links##style", &y.keepLinks);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("hyphenate", &y.hyphenate);
    static const std::vector<std::pair<TableHandling, std::string>> tables = {
        {TableHandling::Keep, "keep tables"}, {TableHandling::Linearize, "linearize (one cell per line)"},
        {TableHandling::Drop, "drop tables"}};
    ch |= ui::EnumCombo("tables", y.tableHandling, tables);
    ImGui::TextDisabled("custom css (added after the preset css)");
    ch |= ui::InputStrMultiline("##css", y.customCss, ImVec2(-1, ImGui::GetTextLineHeight() * 6));
    return ch;
}

void drawConversion(App& app) {
    if (!header("Conversion", "style preset font margin line height paragraph image jpeg grayscale hyphenate table "
                              "css extract wikipedia infobox references links"))
        return;
    bool ch = false;
    if (resetButton("conversion")) {
        app.settings.style = StyleSettings{};
        app.settings.extract = ExtractOptions{};
        ch = true;
    }
    ImGui::PushItemWidth(ImGui::GetFontSize() * 14.f);
    ch |= drawStyle(app);
    ImGui::PopItemWidth();
    ImGui::Separator();
    ExtractOptions& e = app.settings.extract;
    ch |= ImGui::Checkbox("keep wikipedia infobox", &e.keepInfobox);
    ch |= ImGui::Checkbox("keep references and notes", &e.keepReferences);
    ch |= ImGui::Checkbox("keep links", &e.keepLinks);
    ch |= ImGui::Checkbox("keep images", &e.keepImages);
    ch |= ImGui::Checkbox("keep tables", &e.keepTables);
    ImGui::BeginDisabled(!app.lastInput);
    if (ImGui::Button("extract again with these options") && app.lastInput) jobExtract(app, *app.lastInput);
    ImGui::EndDisabled();
    if (ch) app.markSettingsChanged();
    ImGui::Spacing();
}

void drawOutputFiles(App& app) {
    if (!header("Output & Files", "filename template lowercase length cover source note images cache library "
                                  "export folder directory"))
        return;
    AppSettings& s = app.settings;
    bool ch = false;
    if (resetButton("output")) {
        AppSettings d;
        s.filename = d.filename;
        s.includeCover = d.includeCover;
        s.includeSourceNote = d.includeSourceNote;
        s.includeImages = d.includeImages;
        s.cacheImages = d.cacheImages;
        s.lastExportDir.clear();
        ch = true;
    }
    FilenameRules& r = s.filename;
    ImGui::PushItemWidth(ImGui::GetFontSize() * 14.f);
    ch |= ui::InputStr("filename template", r.templ);
    ui::Hint("Fields: {title} {author} {date} {site}");
    ch |= ImGui::SliderInt("max length", &r.maxLength, 20, 200);
    ImGui::PopItemWidth();
    ch |= ImGui::Checkbox("lowercase", &r.lowercase);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.f);
    ch |= ui::InputStr("replace spaces with", r.spaceReplacement);
    if (app.article) ImGui::TextDisabled("example: %s", output::makeFilename(*app.article, app.format, r).c_str());
    ImGui::Separator();
    ch |= ImGui::Checkbox("include cover page", &s.includeCover);
    ch |= ImGui::Checkbox("include source note", &s.includeSourceNote);
    ch |= ImGui::Checkbox("include images", &s.includeImages);
    ch |= ImGui::Checkbox("cache images in the library", &s.cacheImages);
    ui::Tip("cached images let you re-export and bundle without the network");
    ImGui::TextDisabled("export folder:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", s.lastExportDir.empty() ? "(ask each time)" : s.lastExportDir.c_str());
    if (ImGui::SmallButton("choose...")) {
        if (auto d = pickFolderDialog("Choose the export folder")) {
            s.lastExportDir = *d;
            ch = true;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(s.lastExportDir.empty());
    if (ImGui::SmallButton("clear")) {
        s.lastExportDir.clear();
        ch = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("open")) openFolder(s.lastExportDir);
    ImGui::EndDisabled();
    if (ch) app.markSettingsChanged();
    ImGui::Spacing();
}

// The defaults for a new pack (AppSettings.packLimits). The wizard copies
// them before a scan; step 2 can change them for one pack.
void drawPacks(App& app) {
    if (!header("Spider packs (defaults)", "pack spider depth articles images size delay references infobox site"))
        return;
    PackLimits& l = app.settings.packLimits;
    bool ch = false;
    if (resetButton("packs")) {
        l = PackLimits{};
        ch = true;
    }
    static const std::vector<std::pair<int, std::string>> depths = {
        {0, "0 - root only"}, {1, "1 - root + its links"}, {2, "2 - links of links"}};
    ImGui::PushItemWidth(ImGui::GetFontSize() * 12.f);
    ch |= ui::EnumCombo("depth##packs", l.depth, depths);
    ch |= ImGui::SliderInt("max articles", &l.maxArticles, 1, 500);
    ch |= ImGui::SliderInt("max images per article", &l.maxImagesPerArticle, 0, 50);
    ch |= ImGui::SliderInt("max epub size (mb)", &l.maxTotalEpubSizeMB, 1, 500);
    ch |= ImGui::SliderInt("delay between requests (ms)", &l.requestDelayMs, 0, 10000);
    ImGui::PopItemWidth();
    ch |= ImGui::Checkbox("include references##packs", &l.includeReferences);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("include infobox##packs", &l.includeInfobox);
    ImGui::SameLine();
    ch |= ImGui::Checkbox("same site only (web packs)", &l.sameDomainOnly);
    ui::Hint("A delay of 500 ms or more is polite to Wikipedia. Depth 2 can read many pages.");
    if (ch) app.markSettingsChanged();
    ImGui::Spacing();
}

void drawDeviceNetwork(App& app) {
    if (!header("Device & Network", "helper server port origins web app discovery timeout udp lan")) return;
    AppSettings& s = app.settings;
    static std::string origins;
    static int originsRev = -1;
    bool ch = false;
    if (resetButton("network")) {
        AppSettings d;
        s.helperServerEnabled = d.helperServerEnabled;
        s.helperPort = d.helperPort;
        s.helperAllowedOrigins = d.helperAllowedOrigins;
        app.prefs.discovery_timeout_ms = UiPrefs().discovery_timeout_ms;
        app.markPrefsChanged();
        originsRev = -1;
        ch = true;
    }
    if (originsRev == -1) {
        origins = joinLines(s.helperAllowedOrigins);
        originsRev = 0;
    }
    ch |= ImGui::Checkbox("run the helper server for the web app", &s.helperServerEnabled);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.f);
    if (ImGui::InputInt("port", &s.helperPort, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (s.helperPort < 1024 || s.helperPort > 65535) s.helperPort = 8134;
        ch = true;
    }
    ImGui::TextDisabled("allowed origins (one per line)");
    if (ui::InputStrMultiline("##origins", origins, ImVec2(-1, ImGui::GetTextLineHeight() * 4))) {
        s.helperAllowedOrigins = splitLines(origins);
        ch = true;
    }
    ui::Hint("Listens on 127.0.0.1 only. The origin \"null\" is never allowed.");
    ImGui::TextColored(app.helper.running() ? theme::Live : theme::Dim, "%s", app.helperStatus.c_str());
    if (ch) app.markSettingsChanged(true);
    ImGui::Separator();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    if (ImGui::SliderInt("discovery timeout (ms)", &app.prefs.discovery_timeout_ms, 300, 10000)) {
        app.prefs.clamp();
        app.markPrefsChanged();
    }
    ui::Tip("how long discover waits for UDP replies on port 8134");
    ImGui::Spacing();
}

void drawSystem(App& app) {
    if (!header("System", "data folder reduce motion animation tooltips toast notification reset all")) return;
    if (resetButton("system")) {
        app.prefs.resetSystem();
        app.styleDirty = true;
        app.markPrefsChanged();
    }
    static std::string dir = dataDir();
    ImGui::TextDisabled("data folder:");
    ImGui::SameLine();
    ImGui::TextWrapped("%s", dir.c_str());
    if (ImGui::SmallButton("open data folder")) openFolder(dir);
    bool ch = false;
    ch |= ImGui::Checkbox("reduce motion (no toast slide or fade)", &app.prefs.reduce_motion);
    ch |= ImGui::Checkbox("show tooltips", &app.prefs.show_tooltips);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.f);
    ch |= ImGui::SliderFloat("toast time (s)", &app.prefs.toast_seconds, 1.f, 15.f, "%.1f");
    ui::Tip("errors stay twice as long");
    if (ch) {
        app.prefs.clamp();
        app.styleDirty = true;
        app.markPrefsChanged();
    }
    ImGui::TextDisabled("sample toast:");
    ImGui::SameLine();
    if (ImGui::SmallButton("info")) app.toast(ToastKind::Info, "this is an info toast");
    ImGui::SameLine();
    if (ImGui::SmallButton("warn")) app.toast(ToastKind::Warn, "this is a warning toast");
    ImGui::SameLine();
    if (ImGui::SmallButton("error")) app.toast(ToastKind::Error, "this is an error toast");
    ImGui::Separator();
    if (ImGui::Button("reset everything...")) ImGui::OpenPopup("Reset everything?");
    ui::Tip("all settings and UI prefs. devices and the library stay.");
    if (ImGui::BeginPopupModal("Reset everything?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Reset all settings and UI prefs to the defaults?");
        ui::Hint("Device profiles, the library and the window position stay.");
        if (ImGui::Button("reset")) {
            WindowGeom g = app.prefs.window;
            std::string dev = app.settings.defaultDeviceId;
            app.prefs = UiPrefs{};
            app.prefs.window = g;
            app.settings = AppSettings{};
            app.settings.defaultDeviceId = dev;
            app.applyPreset("dark");
            app.fontsDirty = true;
            app.markSettingsChanged(true);
            app.toast(ToastKind::Info, "all settings reset to defaults");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Spacing();
}

void drawAbout(App& app) {
    if (!header("About", "version license credits")) return;
    ImGui::TextColored(theme::Amber, "k-rosspointSender");
    ImGui::SameLine();
    ImGui::TextDisabled("desktop-1.0, imgui %s", IMGUI_VERSION);
    ui::Hint("Articles to EPUB for the Xteink X4 / CrossPoint reader. No telemetry.");
    ImGui::TextDisabled("ui prefs: %s", prefsPath().c_str());
    ImGui::TextDisabled("font: %s%s", app.fontName.c_str(), app.fontsCrisp ? " (ttf)" : "");
    ui::Hint("Look ported from weatherDesktop (MIT). Settings, themes and shortcuts ported from the k-os client "
             "(same author, relicensed MIT).");
    ImGui::Spacing();
}

} // namespace

void drawSettingsWindow(App& app) {
    if (!app.showSettings) return;
    float fs = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(fs * 40.f, fs * 46.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &app.showSettings)) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-1);
    ui::InputStrHint("##settings_filter", "search settings...", settings::filterText());
    ImGui::Spacing();

    // The scroll area leaves room for the fixed footer.
    // Footer: one separator and one row of buttons.
    float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.f + 2.f;
    ImGui::BeginChild("##settings_scroll", ImVec2(0, -footerH));
    settings::drawAppearance(app);
    settings::drawShortcuts(app);
    drawConversion(app);
    drawOutputFiles(app);
    drawPacks(app);
    drawDeviceNetwork(app);
    drawSystem(app);
    drawAbout(app);
    ImGui::EndChild();

    ImGui::Separator();
    bool pending = app.settingsDirty || app.prefsDirty;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", pending ? "saving soon..." : "all changes saved");
    ui::RightAlign(ui::ButtonsWidth({"save", "close"}));
    if (ImGui::Button("save")) {
        app.flushSaves();
        app.toast(ToastKind::Info, "settings saved");
    }
    ui::Tip("write the settings now (they also save on their own)");
    ImGui::SameLine();
    if (ImGui::Button("close")) app.showSettings = false;
    ImGui::End();
}

} // namespace ks::app
