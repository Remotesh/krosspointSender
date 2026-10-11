// k-rosspointSender desktop: shared types.
// This header is the contract between modules. Do not change it without
// updating docs/DESKTOP.md and every module that uses it.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ks {

// ------------------------------------------------------------- input

enum class InputKind { Url, Wikipedia, Html, Text, Markdown, Bookmarklet };

struct RawInput {
    InputKind kind = InputKind::Html;
    std::string content;              // fetched or pasted content
    std::optional<std::string> sourceUrl;
    std::optional<std::string> fileName;
};

// ------------------------------------------------------------- extraction

struct ExtractOptions {
    bool keepInfobox = false;
    bool keepReferences = false;
    bool keepLinks = true;
    bool keepImages = true;
    bool keepTables = true;
};

struct TocEntry {
    std::string id;    // element id inside bodyHtml
    int level = 1;     // 1..6
    std::string text;
};

struct ArticleImage {
    std::string src;   // absolute URL or data: URL
    std::string alt;
};

// A cleaned, sanitized article. bodyHtml is safe, linear, well-formed XHTML
// fragment (every tag closed, void tags self-closed, entities escaped).
struct Article {
    std::string id;            // uuid
    std::string title;
    std::string author;
    std::string publishedAt;   // ISO date or empty
    std::string capturedAt;    // ISO date
    std::string sourceUrl;
    std::string siteName;
    std::string excerpt;
    std::string language = "en";
    std::vector<std::string> tags;
    std::string bodyHtml;
    std::vector<TocEntry> toc;
    std::vector<ArticleImage> images;
    int wordCount = 0;
    InputKind inputKind = InputKind::Html;
};

// ------------------------------------------------------------- output

enum class OutputFormat { Epub, Html, Txt, Md, Zip };
enum class StylePreset { Compact, Comfortable, Large, Reference };
enum class TableHandling { Keep, Linearize, Drop };

struct StyleSettings {
    StylePreset preset = StylePreset::Comfortable;
    int fontSizePercent = 100;
    double marginEm = 0.8;
    double lineHeight = 1.45;
    double paragraphSpacingEm = 0.6;
    int imageMaxWidthPx = 480;
    bool grayscaleImages = true;
    bool keepLinks = true;
    bool hyphenate = false;
    TableHandling tableHandling = TableHandling::Keep;
    int imageQuality = 85;     // JPEG 1..100
    std::string customCss;
};

struct FilenameRules {
    std::string templ = "{title}";   // {title} {author} {date} {site}
    int maxLength = 80;
    bool lowercase = false;
    std::string spaceReplacement;    // "" keeps spaces
};

struct BuildOptions {
    StyleSettings style;
    bool includeImages = true;
    bool includeCover = true;
    bool includeSourceNote = true;
};

struct OutputFile {
    std::string name;      // final filename with extension
    std::string mime;
    std::string bytes;     // file content
    OutputFormat format = OutputFormat::Epub;
    int estimatedPages = 0;
};

// Image bytes keyed by ArticleImage.src.
using ImageMap = std::map<std::string, std::string>;

// ------------------------------------------------------------- devices and transfer

enum class FirmwareType { CrossPoint, KReader, Other };

enum class TransferMethod {
    WebDav,          // PUT on port 80, overwrites
    CrossPointHttp,  // POST /upload?path= multipart; rejects duplicates
    CrossPointWs,    // WebSocket upload on port 81
    Folder,          // write into a local folder (USB drive mode)
    Ble              // future K-Reader BLE (mock for now)
};

struct DeviceProfile {
    std::string id;
    std::string name;
    FirmwareType firmware = FirmwareType::CrossPoint;
    std::string host = "crosspoint.local";
    int port = 80;
    TransferMethod method = TransferMethod::CrossPointHttp;
    std::string targetFolder = "/Books/K-rosspoint/";
    std::string username;
    std::string password;
    OutputFormat defaultFormat = OutputFormat::Epub;
    std::string lastConnectedAt;
    std::string notes;
    std::string createdAt;
    std::string updatedAt;
};

enum class TransferState { Idle, Connecting, Uploading, Verifying, Complete, Failed };

struct TransferProgress {
    TransferState state = TransferState::Idle;
    double fraction = -1;          // 0..1, or -1 when unknown
    uint64_t bytesSent = 0;
    uint64_t bytesTotal = 0;
    std::string message;
};

enum class TransferErrorCode { Network, NotFound, Auth, Exists, Unsupported, Cancelled, Device, Unknown };

struct TransferError {
    TransferErrorCode code = TransferErrorCode::Unknown;
    std::string message;
};

struct TransferResult {
    bool ok = false;
    std::string remotePath;
    uint64_t bytes = 0;
    std::string startedAt;
    std::string finishedAt;
    std::optional<TransferError> error;
};

struct ConnectionTestResult {
    bool ok = false;
    int latencyMs = -1;
    std::string info;              // device-reported JSON or text
    std::optional<TransferError> error;
};

using ProgressFn = std::function<void(const TransferProgress&)>;

// Every transport implements this. Nothing fails silently: a failed send
// returns ok=false with an error, and also reports state Failed on the
// progress callback. `cancel` is read between chunks.
class Transport {
public:
    virtual ~Transport() = default;
    virtual TransferMethod method() const = 0;
    virtual bool isSupported() const = 0;
    virtual std::vector<std::string> limitations() const = 0;
    virtual ConnectionTestResult testConnection(const DeviceProfile& p) = 0;
    virtual TransferResult send(const DeviceProfile& p, const OutputFile& file,
                                const ProgressFn& onProgress, const bool* cancel = nullptr) = 0;
};

// ------------------------------------------------------------- storage

struct TransferRecord {
    std::string id;
    std::string articleId;
    std::string deviceId;
    std::string deviceName;
    TransferMethod method = TransferMethod::WebDav;
    OutputFormat format = OutputFormat::Epub;
    std::string fileName;
    uint64_t bytes = 0;
    bool ok = false;
    std::string error;
    std::string at;
};

struct LibraryEntry {
    Article article;
    std::string fileName;
    OutputFormat lastFormat = OutputFormat::Epub;
    std::vector<TransferRecord> transfers;
    std::string savedAt;
    std::string updatedAt;
    // Image bytes live on disk under <data>/library/<id>/images/, not here.
};

// ------------------------------------------------------------- spider packs (mirror of the web types)

enum class PackSourceKind { Wikipedia, Web };

struct PackCandidate {
    std::string key;        // Wikipedia canonical title, or absolute URL for web
    std::string title;
    std::string url;
    int depth = 0;          // 0 root, 1 linked from root, 2 linked from depth 1
    std::string parentKey;
    int linkCount = 0;      // how many scanned pages link to it
    bool selected = false;
    bool disambiguation = false;
    bool redirect = false;
    bool manual = false;
};

struct PackLimits {
    int depth = 1;                  // 0, 1, 2
    int maxArticles = 20;
    int maxImagesPerArticle = 2;
    int maxTotalEpubSizeMB = 25;
    int requestDelayMs = 500;
    bool includeReferences = false;
    bool includeInfobox = false;
    bool sameDomainOnly = true;     // web spider
};

enum class PackPageState { Queued, Fetching, Done, Failed, Skipped };

struct PackPage {
    PackCandidate candidate;
    PackPageState state = PackPageState::Queued;
    std::optional<Article> article;
    std::string error;
    std::string fileName;           // "steam_engine.xhtml", set by rewriteLinks
};

enum class PackPhase { Idle, Scanning, Fetching, Building, Complete, Failed, Cancelled };

struct PackError { std::string key, title, error; };

struct PackProgress {
    PackPhase phase = PackPhase::Idle;
    int done = 0;
    int total = 0;
    std::string currentTitle;
    std::string message;
    std::vector<PackError> errors;
};

struct PackExternalLink { std::string key, title, url, fromKey; };

struct Pack {
    std::string id;
    PackSourceKind kind = PackSourceKind::Wikipedia;
    std::string title;
    std::string author;             // empty = "Wikipedia contributors" for Wikipedia packs
    std::string rootKey;
    std::string rootUrl;
    PackLimits limits;
    std::vector<PackPage> pages;
    std::vector<PackExternalLink> externalLinks;
    std::string createdAt;
    std::string updatedAt;
};

using PackProgressFn = std::function<void(const PackProgress&)>;

std::string toString(PackSourceKind k);
std::string toString(PackPageState s);
std::string toString(PackPhase p);
std::optional<PackSourceKind> packKindFrom(const std::string& s);
std::optional<PackPageState> packStateFrom(const std::string& s);

// ------------------------------------------------------------- settings

struct AppSettings {
    StyleSettings style;
    FilenameRules filename;
    bool includeImages = true;
    bool includeCover = true;
    bool includeSourceNote = true;
    ExtractOptions extract;
    std::string defaultDeviceId;
    bool cacheImages = true;
    bool helperServerEnabled = true;   // serve http://127.0.0.1:8134 for the web app
    int helperPort = 8134;
    std::vector<std::string> helperAllowedOrigins = {"https://k-engine.org", "http://localhost", "http://127.0.0.1"};
    std::string lastExportDir;
    PackLimits packLimits;
};


// ------------------------------------------------------------- small helpers (implemented in src/core/util.cpp)

std::string nowIso();                         // "2026-10-10T16:00:00Z"
std::string newUuid();                        // v4
std::string toString(InputKind k);
std::string toString(OutputFormat f);
std::string toString(TransferMethod m);
std::string toString(FirmwareType f);
std::string toString(StylePreset p);
std::string toString(TransferState s);
std::optional<OutputFormat> outputFormatFrom(const std::string& s);
std::optional<TransferMethod> transferMethodFrom(const std::string& s);
std::optional<FirmwareType> firmwareFrom(const std::string& s);
std::optional<StylePreset> presetFrom(const std::string& s);
std::string extensionFor(OutputFormat f);     // "epub", "html", ...
std::string mimeFor(OutputFormat f);
std::string dataDir();                        // per-user app data dir; created on demand
std::string readFile(const std::string& path, bool* ok = nullptr);
bool writeFile(const std::string& path, const std::string& bytes);    // atomic: tmp + rename
bool makeDirs(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);
std::string xmlEscape(const std::string& s);
std::string toLower(std::string s);
std::string trim(const std::string& s);

} // namespace ks
