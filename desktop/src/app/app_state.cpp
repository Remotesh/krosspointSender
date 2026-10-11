// App state, job queue and small helpers.
#include "app/app.h"

#include <SDL.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

namespace ks::app {

namespace {
constexpr size_t kLogMax = 2000;
constexpr size_t kToastMax = 6;
// The SDL user event that wakes the loop. 0 means "not registered".
std::atomic<Uint32> g_wakeEvent{0};
} // namespace

// Ported from the k-os client (same author), relicensed MIT: request_frame().
void initWakeEvent() {
    Uint32 t = SDL_RegisterEvents(1);
    if (t != static_cast<Uint32>(-1)) g_wakeEvent = t;
}

void requestFrame() {
    Uint32 t = g_wakeEvent.load();
    if (t == 0) return;
    SDL_Event ev{};
    ev.type = t;
    SDL_PushEvent(&ev);   // thread-safe; a failure only adds latency
}

double appClock() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

std::string clockTime() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[16];
    std::strftime(buf, sizeof buf, "%H:%M:%S", &tmv);
    return buf;
}

void Shared::post(UiTask t) {
    {
        std::lock_guard<std::mutex> lk(mu);
        tasks.push_back(std::move(t));
    }
    requestFrame();
}

void Shared::log(const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back({clockTime(), text});
    }
    requestFrame();
}

App::App() : shared(std::make_shared<Shared>()) {}

void App::addLog(const std::string& text) {
    log.push_back({clockTime(), text});
    while (log.size() > kLogMax) log.pop_front();
}

void App::drain() {
    std::vector<UiTask> tasks;
    std::vector<LogLine> logs;
    {
        std::lock_guard<std::mutex> lk(shared->mu);
        tasks.swap(shared->tasks);
        logs.swap(shared->logs);
    }
    for (auto& l : logs) {
        log.push_back(std::move(l));
        while (log.size() > kLogMax) log.pop_front();
    }
    for (auto& t : tasks) {
        try {
            t(*this);
        } catch (const std::exception& e) {
            addLog(std::string("UI task error: ") + e.what());
        } catch (...) {
            addLog("UI task error: unknown exception");
        }
    }
    // Remove the finished jobs.
    std::vector<std::shared_ptr<JobCtl>> keep;
    for (auto& j : jobs)
        if (!j->done) keep.push_back(j);
    jobs.swap(keep);
}

std::shared_ptr<JobCtl> App::startJob(JobKind kind, const std::string& label,
                                      std::function<void(JobCtx&)> body) {
    auto ctl = std::make_shared<JobCtl>();
    ctl->id = nextJobId++;
    ctl->kind = kind;
    ctl->label = label;
    jobs.push_back(ctl);
    if (kind != JobKind::Quiet) addLog("Start: " + label);
    auto sh = shared;
    std::thread([ctl, sh, body = std::move(body)]() {
        JobCtx ctx;
        ctx.ctl = ctl;
        ctx.sh = sh;
        ctx.cancelFn = [ctl]() { return ctl->cancel.load(); };
        try {
            body(ctx);
        } catch (const std::exception& e) {
            sh->log(ctl->label + " failed: " + e.what());
            std::string msg = e.what();
            JobKind k = ctl->kind;
            sh->post([k, msg](App& app) {
                if (k == JobKind::Source) app.sourceError = msg;
                else if (k == JobKind::Export) app.exportStatus = "Export failed: " + msg;
                else app.statusMsg = msg;
                app.toast(ToastKind::Error, msg);
            });
        } catch (...) {
            sh->log(ctl->label + " failed: unknown exception");
        }
        ctl->done = true;
        requestFrame();
    }).detach();
    return ctl;
}

bool App::busy(JobKind kind) const {
    for (auto& j : jobs)
        if (j->kind == kind && !j->done) return true;
    return false;
}

void App::cancelAll() {
    for (auto& j : jobs) j->requestCancel();
}

void App::markSettingsChanged(bool helperToo) {
    ++settingsRev;
    settingsDirty = true;
    if (helperToo) helperDirty = true;
    settingsChangedAt = appClock();
}

void App::markPrefsChanged() {
    prefsDirty = true;
    prefsChangedAt = appClock();
}

void App::toast(ToastKind kind, const std::string& text) {
    toasts.push_back({nextToastId++, kind, text, appClock()});
    while (toasts.size() > kToastMax) toasts.erase(toasts.begin());
}

void App::applyPreset(const std::string& preset) {
    if (preset == "light") theme = Theme::light_default();
    else if (preset == "korra") theme = Theme::korra_default();
    else if (preset == "custom") {
        Theme t;
        if (t.load_from_file(customThemePath())) theme = t;
    } else if (preset.rfind("file:", 0) == 0) {
        Theme t;
        if (!t.load_from_file(preset.substr(5))) {
            addLog("Cannot read the theme file " + preset.substr(5) + ". Dark theme used.");
            applyPreset("dark");
            return;
        }
        theme = t;
    } else theme = Theme::dark_default();
    prefs.theme_preset = preset;
    styleDirty = true;
    markPrefsChanged();
}

void App::flushSaves() {
    if (settingsDirty) {
        settingsDirty = false;
        try {
            net::saveSettings(settings);
        } catch (const std::exception& e) {
            addLog(std::string("Cannot save the settings: ") + e.what());
        }
    }
    if (helperDirty) {
        helperDirty = false;
        restartHelper();
    }
    if (prefsDirty) {
        prefsDirty = false;
        if (!savePrefs(prefs)) addLog("Cannot save " + prefsPath());
    }
    if (customThemeDirty) {
        customThemeDirty = false;
        theme.save_to_file(customThemePath());
    }
}

// Save the settings 0.6 s after the last change. Restart the helper server
// when its fields changed. Save the UI prefs 0.7 s after the last change.
void App::tick(double now) {
    if (prefsDirty && now - prefsChangedAt >= 0.7) {
        prefsDirty = false;
        UiPrefs copy = prefs;
        std::string themeJson = customThemeDirty ? theme.to_json() : std::string();
        customThemeDirty = false;
        startJob(JobKind::Quiet, "Save UI prefs", [copy, themeJson](JobCtx& ctx) {
            if (!savePrefs(copy)) ctx.log("Cannot save " + prefsPath());
            if (!themeJson.empty()) writeFile(customThemePath(), themeJson);
        });
    }
    if (!settingsDirty || now - settingsChangedAt < 0.6) return;
    settingsDirty = false;
    AppSettings copy = settings;
    startJob(JobKind::Quiet, "Save settings", [copy](JobCtx& ctx) {
        net::saveSettings(copy);
        ctx.log("Settings saved.");
    });
    if (helperDirty) {
        helperDirty = false;
        restartHelper();
    }
}

void App::restartHelper() {
    if (helper.running()) {
        helper.stop();
        addLog("Helper server stopped.");
    }
    if (!settings.helperServerEnabled) {
        helperStatus = "Helper: off";
        return;
    }
    if (helper.start(settings.helperPort, settings.helperAllowedOrigins)) {
        helperStatus = "Helper: on 127.0.0.1:" + std::to_string(settings.helperPort);
        addLog("Helper server listens on 127.0.0.1:" + std::to_string(settings.helperPort) + ".");
    } else {
        helperStatus = "Helper: failed (" + helper.lastError() + ")";
        addLog("Helper server did not start: " + helper.lastError());
    }
}

void App::reloadProfiles() {
    try {
        profiles = net::listProfiles();
    } catch (const std::exception& e) {
        addLog(std::string("Cannot read devices: ") + e.what());
        profiles.clear();
    }
    bool found = false;
    for (auto& p : profiles)
        if (p.id == profileId) found = true;
    if (!found) {
        profileId.clear();
        for (auto& p : profiles)
            if (p.id == settings.defaultDeviceId) profileId = p.id;
        if (profileId.empty() && !profiles.empty()) profileId = profiles.front().id;
        for (auto& p : profiles)
            if (p.id == profileId) method = p.method;
    }
}

std::optional<DeviceProfile> App::currentProfile() const {
    for (auto& p : profiles) {
        if (p.id != profileId) continue;
        DeviceProfile c = p;
        c.method = method;
        return c;
    }
    return std::nullopt;
}

bool App::deviceConfirmed() const {
    for (auto& p : profiles)
        if (p.id == profileId) return !p.lastConnectedAt.empty();
    return false;
}

BuildOptions App::buildOptions() const {
    BuildOptions o;
    o.style = settings.style;
    o.includeImages = settings.includeImages;
    o.includeCover = settings.includeCover;
    o.includeSourceNote = settings.includeSourceNote;
    return o;
}

void App::setArticle(Article a, std::vector<PreviewLine> lines) {
    tagsInput.clear();
    for (size_t i = 0; i < a.tags.size(); ++i) tagsInput += (i ? ", " : "") + a.tags[i];
    article = std::move(a);
    preview = std::move(lines);
    loadedAt = clockTime().substr(0, 5);
    ++articleRev;
    images.clear();
    imagesFetched = false;
    lastError.reset();
    exportStatus.clear();
    sendStatus.clear();
    if (article->images.empty() || !settings.includeImages) imagesFetched = article->images.empty();
    else jobImages(*this);
}

// ---------------------------------------------------------- labels

std::string methodLabel(TransferMethod m) {
    switch (m) {
        case TransferMethod::WebDav: return "WebDAV (port 80, overwrites)";
        case TransferMethod::CrossPointHttp: return "CrossPoint HTTP upload";
        case TransferMethod::CrossPointWs: return "CrossPoint WebSocket (port 81)";
        case TransferMethod::Folder: return "Local folder (USB drive)";
        case TransferMethod::Ble: return "Bluetooth (K-Reader, simulated)";
    }
    return toString(m);
}

std::string formatLabel(OutputFormat f) {
    switch (f) {
        case OutputFormat::Epub: return "EPUB";
        case OutputFormat::Html: return "HTML";
        case OutputFormat::Txt: return "TXT";
        case OutputFormat::Md: return "Markdown";
        case OutputFormat::Zip: return "ZIP";
    }
    return toString(f);
}

std::string stateLabel(TransferState s) {
    switch (s) {
        case TransferState::Idle: return "Idle";
        case TransferState::Connecting: return "Connecting";
        case TransferState::Uploading: return "Uploading";
        case TransferState::Verifying: return "Verifying";
        case TransferState::Complete: return "Complete";
        case TransferState::Failed: return "Failed";
    }
    return "Idle";
}

std::string humanBytes(uint64_t n) {
    char buf[32];
    if (n < 1024) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(n));
    else if (n < 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(n) / 1024.0);
    else std::snprintf(buf, sizeof buf, "%.2f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    return buf;
}

OutputFile buildFile(const Article& a, OutputFormat f, const BuildOptions& o, const ImageMap& images) {
    switch (f) {
        case OutputFormat::Epub: return output::buildEpub(a, o, images);
        case OutputFormat::Html: return output::buildHtml(a, o);
        case OutputFormat::Txt: return output::buildTxt(a, o);
        case OutputFormat::Md: return output::buildMarkdown(a, o);
        case OutputFormat::Zip: return output::buildZip(a, o, images);
    }
    return output::buildEpub(a, o, images);
}

} // namespace ks::app
