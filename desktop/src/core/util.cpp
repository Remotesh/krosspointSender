// Shared helpers declared in core/types.h.
#include "core/types.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

namespace ks {

std::string nowIso() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string newUuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> d(0, 0xffffffffu);
    uint32_t a = d(rng), b = d(rng), c = d(rng), e = d(rng);
    b = (b & 0xffff0fffu) | 0x00004000u;   // version 4
    c = (c & 0x3fffffffu) | 0x80000000u;   // variant
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%04x%08x",
                  a, (b >> 16) & 0xffff, b & 0xffff, (c >> 16) & 0xffff, c & 0xffff, e);
    return buf;
}

std::string toString(InputKind k) {
    switch (k) {
        case InputKind::Url: return "url";
        case InputKind::Wikipedia: return "wikipedia";
        case InputKind::Html: return "html";
        case InputKind::Text: return "text";
        case InputKind::Markdown: return "markdown";
        case InputKind::Bookmarklet: return "bookmarklet";
    }
    return "html";
}

std::string toString(OutputFormat f) {
    switch (f) {
        case OutputFormat::Epub: return "epub";
        case OutputFormat::Html: return "html";
        case OutputFormat::Txt: return "txt";
        case OutputFormat::Md: return "md";
        case OutputFormat::Zip: return "zip";
    }
    return "epub";
}

std::string toString(TransferMethod m) {
    switch (m) {
        case TransferMethod::WebDav: return "webdav";
        case TransferMethod::CrossPointHttp: return "crosspoint-http";
        case TransferMethod::CrossPointWs: return "crosspoint-ws";
        case TransferMethod::Folder: return "folder";
        case TransferMethod::Ble: return "ble";
    }
    return "webdav";
}

std::string toString(FirmwareType f) {
    switch (f) {
        case FirmwareType::CrossPoint: return "crosspoint";
        case FirmwareType::KReader: return "kreader";
        case FirmwareType::Other: return "other";
    }
    return "crosspoint";
}

std::string toString(StylePreset p) {
    switch (p) {
        case StylePreset::Compact: return "compact";
        case StylePreset::Comfortable: return "comfortable";
        case StylePreset::Large: return "large";
        case StylePreset::Reference: return "reference";
    }
    return "comfortable";
}

std::string toString(TransferState s) {
    switch (s) {
        case TransferState::Idle: return "idle";
        case TransferState::Connecting: return "connecting";
        case TransferState::Uploading: return "uploading";
        case TransferState::Verifying: return "verifying";
        case TransferState::Complete: return "complete";
        case TransferState::Failed: return "failed";
    }
    return "idle";
}

std::optional<OutputFormat> outputFormatFrom(const std::string& s) {
    for (auto f : {OutputFormat::Epub, OutputFormat::Html, OutputFormat::Txt, OutputFormat::Md, OutputFormat::Zip})
        if (toString(f) == s) return f;
    return std::nullopt;
}

std::optional<TransferMethod> transferMethodFrom(const std::string& s) {
    for (auto m : {TransferMethod::WebDav, TransferMethod::CrossPointHttp, TransferMethod::CrossPointWs,
                   TransferMethod::Folder, TransferMethod::Ble})
        if (toString(m) == s) return m;
    if (s == "http") return TransferMethod::CrossPointHttp;
    if (s == "usb") return TransferMethod::Folder;
    return std::nullopt;
}

std::optional<FirmwareType> firmwareFrom(const std::string& s) {
    for (auto f : {FirmwareType::CrossPoint, FirmwareType::KReader, FirmwareType::Other})
        if (toString(f) == s) return f;
    return std::nullopt;
}

std::optional<StylePreset> presetFrom(const std::string& s) {
    for (auto p : {StylePreset::Compact, StylePreset::Comfortable, StylePreset::Large, StylePreset::Reference})
        if (toString(p) == s) return p;
    return std::nullopt;
}

std::string toString(PackSourceKind k) { return k == PackSourceKind::Web ? "web" : "wikipedia"; }

std::string toString(PackPageState s) {
    switch (s) {
        case PackPageState::Queued: return "queued";
        case PackPageState::Fetching: return "fetching";
        case PackPageState::Done: return "done";
        case PackPageState::Failed: return "failed";
        case PackPageState::Skipped: return "skipped";
    }
    return "queued";
}

std::string toString(PackPhase p) {
    switch (p) {
        case PackPhase::Idle: return "idle";
        case PackPhase::Scanning: return "scanning";
        case PackPhase::Fetching: return "fetching";
        case PackPhase::Building: return "building";
        case PackPhase::Complete: return "complete";
        case PackPhase::Failed: return "failed";
        case PackPhase::Cancelled: return "cancelled";
    }
    return "idle";
}

std::optional<PackSourceKind> packKindFrom(const std::string& s) {
    if (s == "web") return PackSourceKind::Web;
    if (s == "wikipedia") return PackSourceKind::Wikipedia;
    return std::nullopt;
}

std::optional<PackPageState> packStateFrom(const std::string& s) {
    for (auto v : {PackPageState::Queued, PackPageState::Fetching, PackPageState::Done, PackPageState::Failed, PackPageState::Skipped})
        if (toString(v) == s) return v;
    return std::nullopt;
}

std::string extensionFor(OutputFormat f) { return toString(f); }

std::string mimeFor(OutputFormat f) {
    switch (f) {
        case OutputFormat::Epub: return "application/epub+zip";
        case OutputFormat::Html: return "text/html";
        case OutputFormat::Txt: return "text/plain";
        case OutputFormat::Md: return "text/markdown";
        case OutputFormat::Zip: return "application/zip";
    }
    return "application/octet-stream";
}

std::string dataDir() {
    if (const char* env = std::getenv("KS_DATA_DIR"); env && *env) {
        makeDirs(env);
        return env;
    }
    std::string base;
#ifdef _WIN32
    if (const char* ad = std::getenv("APPDATA"); ad && *ad) base = ad;
    else base = ".";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) base = x;
    else if (const char* h = std::getenv("HOME"); h && *h) base = std::string(h) + "/.local/share";
    else base = ".";
#endif
    std::string dir = joinPath(base, "krosspoint-sender");
    makeDirs(dir);
    return dir;
}

std::string readFile(const std::string& path, bool* ok) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& bytes) {
    std::error_code ec;
    fs::path p = fs::u8path(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        // Windows: rename over an existing file can fail. Remove, then retry.
        fs::remove(p, ec);
        ec.clear();
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

bool makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(fs::u8path(path), ec);
    return !ec || fs::is_directory(fs::u8path(path));
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

} // namespace ks
