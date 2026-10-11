// Persistence: device profiles (devices.json), app settings (settings.json)
// and the article library (library/<id>/entry.json + images/). Packs are
// in packs.cpp.
// All files are JSON in ks::dataDir(). Writes are atomic (ks::writeFile).
// Read errors and bad JSON give the defaults. They never throw.
#include "net/store_json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <regex>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ks::net {

using namespace detail;

namespace {

// ---------------------------------------------------------------- profiles

json profileToJson(const DeviceProfile& p) {
    return {{"id", p.id},
            {"name", p.name},
            {"firmware", ks::toString(p.firmware)},
            {"host", p.host},
            {"port", p.port},
            {"method", ks::toString(p.method)},
            {"targetFolder", p.targetFolder},
            {"username", p.username},
            {"password", p.password},
            {"defaultFormat", ks::toString(p.defaultFormat)},
            {"lastConnectedAt", p.lastConnectedAt},
            {"notes", p.notes},
            {"createdAt", p.createdAt},
            {"updatedAt", p.updatedAt}};
}

DeviceProfile profileFromJson(const json& j) {
    DeviceProfile p;
    p.id = get<std::string>(j, "id", "");
    p.name = get<std::string>(j, "name", "");
    p.firmware = firmwareFrom(get<std::string>(j, "firmware", "")).value_or(FirmwareType::CrossPoint);
    p.host = get<std::string>(j, "host", p.host);
    p.port = get<int>(j, "port", p.port);
    p.method = transferMethodFrom(get<std::string>(j, "method", "")).value_or(TransferMethod::WebDav);
    p.targetFolder = get<std::string>(j, "targetFolder", p.targetFolder);
    p.username = get<std::string>(j, "username", "");
    p.password = get<std::string>(j, "password", "");
    p.defaultFormat = outputFormatFrom(get<std::string>(j, "defaultFormat", "")).value_or(OutputFormat::Epub);
    p.lastConnectedAt = get<std::string>(j, "lastConnectedAt", "");
    p.notes = get<std::string>(j, "notes", "");
    p.createdAt = get<std::string>(j, "createdAt", "");
    p.updatedAt = get<std::string>(j, "updatedAt", "");
    return p;
}

std::string devicesPath() { return joinPath(dataDir(), "devices.json"); }

std::vector<DeviceProfile> loadDevices() {
    std::vector<DeviceProfile> out;
    json j = readJson(devicesPath());
    if (!j.is_array()) return out;
    for (auto& e : j)
        if (e.is_object()) out.push_back(profileFromJson(e));
    return out;
}

void storeDevices(const std::vector<DeviceProfile>& list) {
    json j = json::array();
    for (auto& p : list) j.push_back(profileToJson(p));
    writeJson(devicesPath(), j);
}

bool isLanMethod(TransferMethod m) {
    return m == TransferMethod::WebDav || m == TransferMethod::CrossPointHttp || m == TransferMethod::CrossPointWs;
}

// ---------------------------------------------------------------- article and library


json entryToJson(const LibraryEntry& e) {
    json tr = json::array();
    for (auto& t : e.transfers) tr.push_back(transferToJson(t));
    return {{"article", articleToJson(e.article)},
            {"fileName", e.fileName},
            {"lastFormat", ks::toString(e.lastFormat)},
            {"transfers", tr},
            {"savedAt", e.savedAt},
            {"updatedAt", e.updatedAt}};
}

LibraryEntry entryFromJson(const json& j) {
    LibraryEntry e;
    if (j.contains("article")) e.article = articleFromJson(j["article"]);
    e.fileName = get<std::string>(j, "fileName", "");
    e.lastFormat = outputFormatFrom(get<std::string>(j, "lastFormat", "")).value_or(OutputFormat::Epub);
    if (j.contains("transfers") && j["transfers"].is_array())
        for (auto& t : j["transfers"]) e.transfers.push_back(transferFromJson(t));
    e.savedAt = get<std::string>(j, "savedAt", "");
    e.updatedAt = get<std::string>(j, "updatedAt", "");
    return e;
}

std::string libraryDir() { return joinPath(dataDir(), "library"); }

// The id is a folder name. Accept only safe characters.
void checkId(const std::string& id) {
    if (id.empty()) throw std::runtime_error("The library entry has no article id.");
    if (!safeId(id)) throw std::runtime_error("The article id contains characters that are not allowed: " + id);
}

std::string entryDir(const std::string& id) { return joinPath(libraryDir(), id); }
std::string entryPath(const std::string& id) { return joinPath(entryDir(id), "entry.json"); }

std::optional<LibraryEntry> readEntry(const std::string& id) {
    if (!safeId(id)) return std::nullopt;
    json j = readJson(entryPath(id));
    if (!j.is_object()) return std::nullopt;
    return entryFromJson(j);
}

// ---------------------------------------------------------------- settings

const char* tableToString(TableHandling t) {
    switch (t) {
        case TableHandling::Keep: return "keep";
        case TableHandling::Linearize: return "linearize";
        case TableHandling::Drop: return "drop";
    }
    return "keep";
}

TableHandling tableFrom(const std::string& s) {
    if (s == "linearize") return TableHandling::Linearize;
    if (s == "drop") return TableHandling::Drop;
    return TableHandling::Keep;
}

json styleToJson(const StyleSettings& s) {
    return {{"preset", ks::toString(s.preset)},
            {"fontSizePercent", s.fontSizePercent},
            {"marginEm", s.marginEm},
            {"lineHeight", s.lineHeight},
            {"paragraphSpacingEm", s.paragraphSpacingEm},
            {"imageMaxWidthPx", s.imageMaxWidthPx},
            {"grayscaleImages", s.grayscaleImages},
            {"keepLinks", s.keepLinks},
            {"hyphenate", s.hyphenate},
            {"tableHandling", tableToString(s.tableHandling)},
            {"imageQuality", s.imageQuality},
            {"customCss", s.customCss}};
}

StyleSettings styleFromJson(const json& j) {
    StyleSettings d;
    StyleSettings s;
    s.preset = presetFrom(get<std::string>(j, "preset", "")).value_or(d.preset);
    s.fontSizePercent = get<int>(j, "fontSizePercent", d.fontSizePercent);
    s.marginEm = get<double>(j, "marginEm", d.marginEm);
    s.lineHeight = get<double>(j, "lineHeight", d.lineHeight);
    s.paragraphSpacingEm = get<double>(j, "paragraphSpacingEm", d.paragraphSpacingEm);
    s.imageMaxWidthPx = get<int>(j, "imageMaxWidthPx", d.imageMaxWidthPx);
    s.grayscaleImages = get<bool>(j, "grayscaleImages", d.grayscaleImages);
    s.keepLinks = get<bool>(j, "keepLinks", d.keepLinks);
    s.hyphenate = get<bool>(j, "hyphenate", d.hyphenate);
    s.tableHandling = tableFrom(get<std::string>(j, "tableHandling", "keep"));
    s.imageQuality = std::clamp(get<int>(j, "imageQuality", d.imageQuality), 1, 100);
    s.customCss = get<std::string>(j, "customCss", d.customCss);
    return s;
}

std::string settingsPath() { return joinPath(dataDir(), "settings.json"); }

} // namespace

// ================================================================ profiles API

bool isValidHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    static const std::regex ipv4(R"(^\d+(\.\d+){3}$)");
    static const std::regex label(R"(^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$)");
    if (std::regex_match(host, ipv4)) {
        size_t start = 0;
        while (start <= host.size()) {
            size_t dot = host.find('.', start);
            std::string o = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (o.size() > 3 || std::stoi(o) > 255) return false;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        return true;
    }
    size_t start = 0;
    for (;;) {
        size_t dot = host.find('.', start);
        std::string l = host.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!std::regex_match(l, label)) return false;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return true;
}

DeviceProfile defaultProfile(FirmwareType f) {
    DeviceProfile p;
    p.firmware = f;
    p.port = 80;
    p.targetFolder = "/Books/K-rosspoint/";
    p.defaultFormat = OutputFormat::Epub;
    switch (f) {
        case FirmwareType::KReader:
            p.name = "K-Reader";
            p.host = "";
            p.method = TransferMethod::Ble;
            break;
        case FirmwareType::Other:
            p.name = "Other device";
            p.host = "";
            p.method = TransferMethod::WebDav;
            break;
        case FirmwareType::CrossPoint:
        default:
            p.name = "Xteink X4 Pro (CrossPoint)";
            p.host = "crosspoint.local";
            p.method = TransferMethod::CrossPointHttp;
            break;
    }
    return p;
}

std::vector<std::string> validateProfile(const DeviceProfile& p) {
    std::vector<std::string> problems;
    if (trim(p.name).empty()) problems.push_back("Enter a name for the device.");

    const std::string host = trim(p.host);
    if (p.method == TransferMethod::Folder) {
        // For the Folder method, host is the local folder (the drive root).
        if (host.empty()) problems.push_back("Enter the folder of the reader drive (for example E:/) in the host field.");
    } else if (isLanMethod(p.method) || !host.empty()) {
        static const std::regex scheme(R"(^[a-zA-Z][a-zA-Z0-9+.-]*://)");
        if (host.empty())
            problems.push_back("Enter the device host name or IP address.");
        else if (std::regex_search(host, scheme))
            problems.push_back("Remove the scheme (http://) from the host. Enter only the name or the IP address.");
        else if (std::any_of(host.begin(), host.end(), [](unsigned char c) { return std::isspace(c); }))
            problems.push_back("The host must not contain spaces.");
        else if (!isValidHost(host))
            problems.push_back("The host must be a host name (crosspoint.local) or an IPv4 address (192.168.1.50).");
    }

    if (p.port < 1 || p.port > 65535) problems.push_back("The port must be a whole number from 1 to 65535.");
    if (p.targetFolder.empty() || p.targetFolder[0] != '/') problems.push_back("The target folder must start with \"/\".");
    return problems;
}

std::vector<DeviceProfile> listProfiles() {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    auto list = loadDevices();
    std::stable_sort(list.begin(), list.end(),
                     [](const DeviceProfile& a, const DeviceProfile& b) { return a.createdAt < b.createdAt; });
    return list;
}

std::optional<DeviceProfile> getProfile(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    for (auto& p : loadDevices())
        if (p.id == id) return p;
    return std::nullopt;
}

DeviceProfile saveProfile(DeviceProfile p) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    auto list = loadDevices();
    const std::string now = nowIso();
    auto it = p.id.empty() ? list.end()
                           : std::find_if(list.begin(), list.end(), [&](const DeviceProfile& x) { return x.id == p.id; });
    if (p.id.empty()) p.id = newUuid();
    p.name = trim(p.name);
    p.host = trim(p.host);
    p.targetFolder = trim(p.targetFolder);
    if (it != list.end()) {
        p.createdAt = it->createdAt.empty() ? now : it->createdAt;
        if (p.lastConnectedAt.empty()) p.lastConnectedAt = it->lastConnectedAt;
    } else if (p.createdAt.empty()) {
        p.createdAt = now;
    }
    p.updatedAt = now;
    auto problems = validateProfile(p);
    if (!problems.empty()) {
        std::string msg;
        for (auto& s : problems) msg += (msg.empty() ? "" : " ") + s;
        throw std::runtime_error(msg);
    }
    if (it != list.end()) *it = p;
    else list.push_back(p);
    storeDevices(list);
    return p;
}

void deleteProfile(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    auto list = loadDevices();
    auto n = list.size();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const DeviceProfile& x) { return x.id == id; }), list.end());
    if (list.size() != n) storeDevices(list);
}

void markConnected(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    auto list = loadDevices();
    for (auto& p : list) {
        if (p.id != id) continue;
        p.lastConnectedAt = nowIso();
        p.updatedAt = p.lastConnectedAt;
        storeDevices(list);
        return;
    }
}

// ================================================================ settings API

AppSettings loadSettings() {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    AppSettings d;
    json j = readJson(settingsPath());
    if (!j.is_object()) return d;
    AppSettings s;
    s.style = styleFromJson(j.contains("style") ? j["style"] : json::object());
    json f = j.contains("filename") ? j["filename"] : json::object();
    s.filename.templ = get<std::string>(f, "template", d.filename.templ);
    s.filename.maxLength = get<int>(f, "maxLength", d.filename.maxLength);
    s.filename.lowercase = get<bool>(f, "lowercase", d.filename.lowercase);
    s.filename.spaceReplacement = get<std::string>(f, "spaceReplacement", d.filename.spaceReplacement);
    json b = j.contains("build") ? j["build"] : json::object();
    s.includeImages = get<bool>(b, "includeImages", d.includeImages);
    s.includeCover = get<bool>(b, "includeCover", d.includeCover);
    s.includeSourceNote = get<bool>(b, "includeSourceNote", d.includeSourceNote);
    json e = j.contains("extract") ? j["extract"] : json::object();
    s.extract.keepInfobox = get<bool>(e, "keepInfobox", d.extract.keepInfobox);
    s.extract.keepReferences = get<bool>(e, "keepReferences", d.extract.keepReferences);
    s.extract.keepLinks = get<bool>(e, "keepLinks", d.extract.keepLinks);
    s.extract.keepImages = get<bool>(e, "keepImages", d.extract.keepImages);
    s.extract.keepTables = get<bool>(e, "keepTables", d.extract.keepTables);
    s.defaultDeviceId = get<std::string>(j, "defaultDeviceId", d.defaultDeviceId);
    s.cacheImages = get<bool>(j, "cacheImages", d.cacheImages);
    s.helperServerEnabled = get<bool>(j, "helperServerEnabled", d.helperServerEnabled);
    s.helperPort = get<int>(j, "helperPort", d.helperPort);
    if (s.helperPort < 1 || s.helperPort > 65535) s.helperPort = d.helperPort;
    s.helperAllowedOrigins = getStrings(j, "helperAllowedOrigins", d.helperAllowedOrigins);
    s.lastExportDir = get<std::string>(j, "lastExportDir", d.lastExportDir);
    s.packLimits = packLimitsFromJson(j.contains("packLimits") ? j["packLimits"] : json::object());
    return s;
}

void saveSettings(const AppSettings& s) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    json j = {{"version", 1},
              {"style", styleToJson(s.style)},
              {"filename",
               {{"template", s.filename.templ},
                {"maxLength", s.filename.maxLength},
                {"lowercase", s.filename.lowercase},
                {"spaceReplacement", s.filename.spaceReplacement}}},
              {"build",
               {{"includeImages", s.includeImages},
                {"includeCover", s.includeCover},
                {"includeSourceNote", s.includeSourceNote}}},
              {"extract",
               {{"keepInfobox", s.extract.keepInfobox},
                {"keepReferences", s.extract.keepReferences},
                {"keepLinks", s.extract.keepLinks},
                {"keepImages", s.extract.keepImages},
                {"keepTables", s.extract.keepTables}}},
              {"defaultDeviceId", s.defaultDeviceId},
              {"cacheImages", s.cacheImages},
              {"helperServerEnabled", s.helperServerEnabled},
              {"helperPort", s.helperPort},
              {"helperAllowedOrigins", s.helperAllowedOrigins},
              {"lastExportDir", s.lastExportDir},
              {"packLimits", limitsToJson(s.packLimits)}};
    writeJson(settingsPath(), j);
}

// ================================================================ library API

void saveEntry(const LibraryEntry& in, const ImageMap* images) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    checkId(in.article.id);
    LibraryEntry e = in;
    const std::string now = nowIso();
    auto old = readEntry(e.article.id);
    if (e.savedAt.empty()) e.savedAt = old && !old->savedAt.empty() ? old->savedAt : now;
    if (e.updatedAt.empty()) e.updatedAt = now;
    makeDirs(entryDir(e.article.id));

    if (images) writeImageDir(joinPath(entryDir(e.article.id), "images"), *images);
    writeJson(entryPath(e.article.id), entryToJson(e));
}

std::optional<LibraryEntry> getEntry(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    return readEntry(id);
}

std::vector<LibraryEntry> listEntries() {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    std::vector<LibraryEntry> out;
    std::error_code ec;
    fs::path dir = fs::u8path(libraryDir());
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& d : fs::directory_iterator(dir, ec)) {
        if (!d.is_directory(ec)) continue;
        auto e = readEntry(d.path().filename().u8string());
        if (e) out.push_back(std::move(*e));
    }
    std::sort(out.begin(), out.end(), [](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
        return a.savedAt > b.savedAt;
    });
    return out;
}

ImageMap loadImages(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    if (!safeId(id)) return {};
    return readImageDir(joinPath(entryDir(id), "images"));
}

void deleteEntry(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    if (!safeId(id)) return;
    std::error_code ec;
    fs::remove_all(fs::u8path(entryDir(id)), ec);
}

void clearLibrary() {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    std::error_code ec;
    fs::remove_all(fs::u8path(libraryDir()), ec);
    clearPacks();
}

void addTransfer(const std::string& articleId, const TransferRecord& r) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    auto e = readEntry(articleId);
    if (!e) throw std::runtime_error("No library entry with id " + articleId + ".");
    TransferRecord rec = r;
    rec.articleId = articleId;
    if (rec.id.empty()) rec.id = newUuid();
    if (rec.at.empty()) rec.at = nowIso();
    e->transfers.push_back(rec);
    e->updatedAt = nowIso();
    writeJson(entryPath(articleId), entryToJson(*e));
}

} // namespace ks::net
