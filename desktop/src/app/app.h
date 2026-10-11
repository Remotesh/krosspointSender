// k-rosspointSender desktop: application state, job queue, UI entry points.
// The UI thread owns App. Worker threads never touch App. Workers send
// results back as UiTask closures. The UI thread runs them in drain().
#pragma once

#include "app/prefs.h"
#include "app/theme.h"
#include "core/types.h"
#include "extract/extract.h"
#include "net/net.h"
#include "output/output.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct SDL_Window;

namespace ks::app {

struct App;
using UiTask = std::function<void(App&)>;

// The kind of a job. The panels use it to show the matching spinner.
enum class JobKind { Source, Images, Export, Send, Test, Discover, BleScan, Library, Pack, Folder, Quiet };

// Control block for one background job. The UI and the worker share it.
struct JobCtl {
    int id = 0;
    JobKind kind = JobKind::Quiet;
    std::string label;
    std::atomic<bool> cancel{false};
    // Transport::send reads a plain bool. This mirror holds the same value.
    bool cancelPlain = false;
    std::atomic<float> progress{-1.f};   // 0..1, or -1 when unknown
    std::atomic<bool> done{false};
    void requestCancel() { cancel = true; cancelPlain = true; }
};

struct LogLine {
    std::string time;
    std::string text;
};

// The mailbox between workers and the UI thread.
struct Shared {
    std::mutex mu;
    std::vector<UiTask> tasks;
    std::vector<LogLine> logs;
    void post(UiTask t);
    void log(const std::string& text);
};

// The worker side of a job.
struct JobCtx {
    std::shared_ptr<JobCtl> ctl;
    std::shared_ptr<Shared> sh;
    std::function<bool()> cancelFn;   // for ks::extract calls
    bool cancelled() const { return ctl->cancel.load(); }
    void log(const std::string& s) const { sh->log(s); }
    void post(UiTask t) const { sh->post(std::move(t)); }
    void progress(float f) const { ctl->progress = f; }
};

// One line of the preview. level 0 is body text, 1..6 is a heading.
struct PreviewLine {
    std::string text;
    int level = 0;
};

// The devices window (win_devices.cpp, device_setup.cpp). List shows the
// found and the saved devices. Form is the add / edit flow.
enum class DeviceView { List, Form };

// The destination of a connection test result.
enum class TestTarget { Panel, Form };

// The state of the inline device flow: 1 test, 2 confirm.
struct DeviceFlow {
    DeviceView view = DeviceView::List;
    DeviceProfile draft;
    bool editing = false;                       // the draft is a saved profile
    std::vector<std::string> problems;
    std::optional<ConnectionTestResult> test;
    std::string testedKey;                      // host:port:method of the test above
    int discoverSel = -1;                       // selected row of App::discovered
    bool autoDiscovered = false;                // the list ran discovery once
    std::string deleteId;                       // profile for the delete popup
    // The destination folder browser (folder_browser.cpp).
    bool browseOpen = false;
    std::string browsePath = "/";
    std::vector<net::RemoteEntry> browseEntries;
    std::string browseError;
    bool browseLoaded = false;
    std::string newFolderName = "K-rosspoint";
    bool newFolderOpen = false;
    std::string bleAddrText;                    // the address field of a Bluetooth draft
    int bleSel = -1;                            // selected row of App::bleFound
};

enum class SourceTab { Url, Wikipedia, Html, Text, Markdown, File, Pack };

// The spider pack wizard and the current pack (pack_wizard.cpp,
// pack_select.cpp, pack_jobs.cpp, panel_pack.cpp).
// Steps: 0 root, 1 scan, 2 select, 3 build, 4 done.
struct PackUi {
    int step = 0;
    bool webSource = false;              // root: "current article + its links"
    std::string wikiInput, wikiLang = "en";
    // The root of step 1.
    bool hasRoot = false;
    PackSourceKind rootKind = PackSourceKind::Wikipedia;
    std::string rootKey, rootTitle, rootUrl, rootLang = "en";
    std::optional<Article> rootArticle;  // web pack: the loaded article
    PackLimits limits;                   // the limits of the next scan
    bool limitsSet = false;              // limits came from the settings
    // The pack after the scan.
    std::optional<Pack> pack;
    std::string scanLine;
    bool scanFailed = false;
    std::string filter, addInput;
    // The build of step 4.
    std::string buildLine;
    float buildFrac = 0.f;
    std::vector<PackError> errors;
    // The built EPUB. It is the output of the right column in pack mode.
    std::optional<OutputFile> file;
    ImageMap images;
    std::string builtTitle, builtAuthor;
    std::string fileName;                // the name the user wants, "" = the built name
    // Center column.
    std::string previewKey;
    std::vector<PreviewLine> previewLines;
    int previewRev = 0;
    int rev = 0;                         // changes when the pack changes
};

// The "send a file" window (send_file.cpp): raw files from disk to the
// reader, no conversion, no library article.
struct SendFileItem {
    std::string path;
    std::string name;       // the remote name; editable for a single file
    uint64_t size = 0;
    bool readable = true;
};
struct SendFileUi {
    bool open = false;
    bool focus = false;                  // bring the window to the front once
    bool dropped = false;                // opened by a drop of a non-article file
    std::vector<SendFileItem> items;
    std::string folder;                  // "" until opened; then the profile folder
    std::string profileFolder;           // the folder of the device when opened
    // The result of the last run.
    int sent = 0, failed = 0;
    bool finished = false;
    bool cancelled = false;
    bool firmwareSent = false;           // a .bin arrived: show the reader hint
    std::vector<std::string> lines;      // one line per file
    std::vector<TransferRecord> recent;  // file_sends.json, newest first
    bool recentLoaded = false;
};

enum class ToastKind { Info, Warn, Error };

// A short message in the top-right overlay.
struct Toast {
    int id = 0;
    ToastKind kind = ToastKind::Info;
    std::string text;
    double born = 0;   // appClock() time
};

struct App {
    App();

    // ---------------------------------------------------------- persistent data
    AppSettings settings;
    std::vector<DeviceProfile> profiles;
    std::string profileId;
    TransferMethod method = TransferMethod::WebDav;

    // ---------------------------------------------------------- UI prefs and theme
    UiPrefs prefs;
    Theme theme;
    std::vector<ThemeFile> themeFiles;
    bool themeFilesScanned = false;
    bool styleDirty = true;    // re-apply the theme and the scale before the next frame
    bool fontsDirty = true;    // rebuild the font atlas before the next frame
    bool prefsDirty = false;
    bool customThemeDirty = false;
    double prefsChangedAt = 0;
    std::vector<Toast> toasts;
    int nextToastId = 1;
    bool fontsCrisp = false;   // fonts.cpp sets it
    std::string fontName;      // the loaded font, for the UI

    // ---------------------------------------------------------- source panel
    SourceTab sourceTab = SourceTab::Url;
    bool selectTab = false;   // true for one frame after code changes the tab
    std::string urlInput, wikiInput, wikiLang = "en";
    std::string pasteHtml, pasteText, pasteMd, filePath;
    std::string sourceStatus;
    std::string sourceError;
    std::vector<std::string> sourceFallbacks;
    std::optional<RawInput> lastInput;
    std::string loadedAt;      // "HH:MM" of the last loaded article

    // ---------------------------------------------------------- article
    std::optional<Article> article;
    std::vector<PreviewLine> preview;
    ImageMap images;
    bool imagesFetched = false;
    std::string tagsInput;
    bool devicePreview = false;
    bool focusUrl = false;     // focus the URL box on the next frame
    int articleRev = 0;        // changes when the article or its metadata changes
    int settingsRev = 0;       // changes when the app settings change

    // ---------------------------------------------------------- spider pack
    PackUi pk;
    bool packMode = false;     // the center and right columns show the pack

    // ---------------------------------------------------------- output and device
    OutputFormat format = OutputFormat::Epub;
    std::string exportStatus;
    TransferProgress transfer;
    std::optional<TransferError> lastError;
    std::string sendStatus;
    std::string lastExplain;   // full text of the last failure: title, reasons, fallbacks
    std::optional<ConnectionTestResult> testResult;
    std::vector<net::Discovered> discovered;
    bool discoverDone = false;
    std::vector<net::BleDiscovered> bleFound;   // Bluetooth readers from the last scan
    bool bleScanDone = false;
    std::string bleScanError;
    DeviceFlow dev;
    SendFileUi sf;

    // ---------------------------------------------------------- windows
    bool showLibrary = false, showSettings = false, showDevices = false, showLog = false;
    std::vector<LibraryEntry> library;
    std::set<std::string> librarySel;
    std::vector<Pack> libraryPacks;
    std::map<std::string, net::PackRecord> packRecords;   // pack id -> file name and sends
    bool libraryLoaded = false;

    // ---------------------------------------------------------- log and jobs
    std::deque<LogLine> log;
    bool logScroll = true;
    std::vector<std::shared_ptr<JobCtl>> jobs;
    std::shared_ptr<Shared> shared;
    int nextJobId = 1;

    // ---------------------------------------------------------- helper server
    net::HelperServer helper;
    std::string helperStatus;

    // ---------------------------------------------------------- deferred saves
    bool settingsDirty = false;
    bool helperDirty = false;
    double settingsChangedAt = 0;
    std::string statusMsg;

    // ---------------------------------------------------------- methods
    void addLog(const std::string& text);
    void drain();
    void tick(double now);
    std::shared_ptr<JobCtl> startJob(JobKind kind, const std::string& label,
                                     std::function<void(JobCtx&)> body);
    bool busy(JobKind kind) const;
    void cancelAll();
    void markSettingsChanged(bool helperToo = false);
    void markPrefsChanged();
    void toast(ToastKind kind, const std::string& text);
    void applyPreset(const std::string& preset);   // load the theme for a theme_preset value
    void flushSaves();                             // write the pending settings and prefs now
    void restartHelper();
    void reloadProfiles();
    std::optional<DeviceProfile> currentProfile() const;   // method combo applied
    bool deviceConfirmed() const;   // a device is selected and it received a file or passed a test
    BuildOptions buildOptions() const;
    void setArticle(Article a, std::vector<PreviewLine> lines);
};

// ---------------------------------------------------------- helpers (app_state.cpp)
std::string methodLabel(TransferMethod m);
std::string formatLabel(OutputFormat f);
std::string stateLabel(TransferState s);
std::string humanBytes(uint64_t n);
// Where a device is: host:port, the drive for Folder, or the Bluetooth name and address.
std::string deviceWhere(const DeviceProfile& p);
std::string clockTime();
double appClock();   // seconds since start, steady
// Wake the event loop from any thread (k-os request_frame). Safe to call
// before initWakeEvent(); then it does nothing.
void initWakeEvent();
void requestFrame();
OutputFile buildFile(const Article& a, OutputFormat f, const BuildOptions& o, const ImageMap& images);
std::vector<PreviewLine> buildPreviewLines(const Article& a);   // panel_preview.cpp

// ---------------------------------------------------------- jobs (jobs.cpp)
void jobLoadUrl(App& app, const std::string& input);
void jobWikipedia(App& app, const std::string& titleOrUrl, const std::string& lang);
void jobExtract(App& app, RawInput in);
void jobLoadFile(App& app, const std::string& path);
void jobImages(App& app);
void jobExport(App& app, const std::string& path);
void jobSend(App& app);
void jobSendFile(App& app, OutputFile file);          // no library entry (test EPUB)
void jobSendFileTo(App& app, const DeviceProfile& p, OutputFile file);
void jobFallbackFolder(App& app, const std::string& folder);
void jobTest(App& app, const DeviceProfile& p, TestTarget target);
// Send the test EPUB to the draft. On success save it, mark it connected,
// make it the default device and show the compact device strip.
void jobConfirmDevice(App& app, const DeviceProfile& draft);
std::string testKey(const DeviceProfile& p);   // host:port:method (Bluetooth: address:method)
void jobDiscover(App& app);
// Send one file with a progress callback that posts to the UI (jobs.cpp).
TransferResult sendWithProgress(const JobCtx& ctx, const DeviceProfile& p, const OutputFile& f);
void postSendResult(const JobCtx& ctx, const TransferResult& res, const std::string& name, const std::string& device,
                    TransferMethod method = TransferMethod::WebDav);
// Scan for CrossPoint readers over Bluetooth LE (about 4 s); fills App::bleFound.
void jobDiscoverBle(App& app);
TransferRecord makeRecord(const std::string& articleId, const DeviceProfile& p, const OutputFile& f,
                          const TransferResult& res);

// ---------------------------------------------------------- spider pack (pack_jobs.cpp)
// Step 1 -> 2: check the root input and set PackUi root fields. Returns an
// error text, or "" when the root is good.
std::string packSetRoot(App& app);
void jobPackScan(App& app);
void jobPackBuild(App& app);
void jobPackExport(App& app, const std::string& path);
void jobPackSend(App& app);
// Library: open, re-export, send a stored pack.
void jobPackOpen(App& app, const std::string& id);
void jobPackReexport(App& app, const std::string& id, const std::string& path);
void jobPackSendStored(App& app, const std::string& id);
// The file name of the pack output: the user's name made safe, with .epub.
std::string packFinalName(const App& app);
// Extract options for pack pages (the pack limits set infobox and references).
ExtractOptions packExtractOptions(const AppSettings& s, const PackLimits& l);
struct PackStats {
    int articles = 0, words = 0, pages = 0, images = 0, failed = 0, shown = 0;
};
PackStats packStats(const Pack& p, const StyleSettings& style);
bool packShown(const PackPage& p);   // selected, done or failed
// Start a new pack: back to step 1 with the default limits.
void packRestart(App& app);

// ---------------------------------------------------------- send a file (send_file.cpp)
// Open the native multi-select dialog, then the send-file window.
void chooseFilesToSend(App& app);
// Open the send-file window with these files (added to the list when the
// window is open and idle). dropped: the files came from a drop.
void openSendFile(App& app, const std::vector<std::string>& paths, bool dropped = false);
// Send the listed files one by one to the current device.
void jobSendRawFiles(App& app);
void drawSendFileWindow(App& app);

// ---------------------------------------------------------- folder browser (folder_browser.cpp)
void drawFolderBrowser(App& app, DeviceProfile& d);

// ---------------------------------------------------------- library (win_library.cpp)
void jobRefreshLibrary(App& app);   // articles and packs
void drawLibraryPacks(App& app);    // win_library_packs.cpp
void saveToLibrary(const JobCtx& ctx, const Article& a, const OutputFile& f, const ImageMap* images,
                   const std::optional<TransferRecord>& rec);

// ---------------------------------------------------------- input routing (panel_source.cpp)
void handleDropFile(App& app, const std::string& path);
void handleDropText(App& app, const std::string& text);
void pasteFromClipboard(App& app);
RawInput rawFromFile(const std::string& path, const std::string& bytes);

// ---------------------------------------------------------- UI (one file each)
// Style and fonts (fonts.cpp). Call between frames.
float displayScale();
void applyStyle(App& app);
void rebuildFonts(App& app);
// Window chrome (chrome.cpp).
void drawMenuBar(App& app);
void drawMainLayout(App& app);
void drawToasts(App& app);
bool toastsAnimating(const App& app);   // a toast slides or fades
// Actions shared by the panels and the shortcuts.
void exportNow(App& app);
void convertCurrent(App& app);
void fetchCurrent(App& app);
void drawSourcePanel(App& app);
void drawPreviewPanel(App& app);
void drawPackWizard(App& app);                 // pack_wizard.cpp: the [ pack ] source tab
void drawPackSelectStep(App& app);             // pack_select.cpp: step 3
void drawPackPreview(App& app);                // panel_pack.cpp: the center column in pack mode
// The text of one article with headings (panel_preview.cpp). rev keys the
// line cache of this call site.
void drawArticleText(const Article& a, const std::vector<PreviewLine>& lines, int rev, float base);
void drawRightPanel(App& app);
void drawDeviceList(App& app);                 // device_setup.cpp
void drawDeviceForm(App& app);                 // device_setup.cpp
void openDeviceForm(App& app, const DeviceProfile& p, bool editing);
// The [ < ] / [ > ] pill that collapses a side column (layout.cpp).
void drawCollapsePill(App& app, bool left);
void drawColumns(App& app, float height);      // layout.cpp
// The device combo, the send button and the transfer pill on the right
// side of the menu bar (topbar.cpp).
void drawDeviceBar(App& app);
// Open the devices window on the list. Discovery runs once if the list is empty.
void openDevicesWindow(App& app);
void drawLibraryWindow(App& app);
void drawSettingsWindow(App& app);
void drawDevicesWindow(App& app);
void drawLogWindow(App& app);

// ---------------------------------------------------------- platform (dialogs.cpp, clipboard_win.cpp)
std::optional<std::string> saveFileDialog(App& app, const std::string& defaultName, OutputFormat f);
std::optional<std::string> openFileDialog();
// Generic open/save dialogs. filterDesc "Font files", pattern "*.ttf;*.otf".
std::optional<std::string> openFileDialogFiltered(const std::string& title, const std::string& filterDesc,
                                                  const std::string& pattern);
std::optional<std::string> saveFileDialogFiltered(const std::string& title, const std::string& filterDesc,
                                                  const std::string& pattern, const std::string& defaultPath);
std::optional<std::string> pickFolderDialog(const std::string& title);
// Multi-select open dialog for any file (GetOpenFileNameW with
// OFN_ALLOWMULTISELECT). Empty when cancelled. supported is false on a
// platform without a native dialog.
std::vector<std::string> openFilesDialog(const std::string& title, bool* supported = nullptr);
void openFolder(const std::string& path);
struct ClipboardContent {
    std::string html;        // CF_HTML document, empty when none
    std::string sourceUrl;   // SourceURL from CF_HTML
    std::string text;        // plain text
};
ClipboardContent readClipboard();
float loadUiScale();   // old ui_scale.txt, read once to migrate into ui_prefs.json
// Tint the native title bar to the theme (Windows 11; no-op elsewhere).
void tintTitleBar(::SDL_Window* win, const Theme& t);

// UI test hook (uitest.cpp). Returns true while it has work for the next frame.
bool uiTestPoll(App& app);
// After the render, before the swap: write a pending "shot" (uitest.cpp).
void uiTestAfterRender(int width, int height);

// ---------------------------------------------------------- CLI (cli.cpp)
bool isCliArgs(int argc, char** argv);
int runCli(int argc, char** argv);

} // namespace ks::app
