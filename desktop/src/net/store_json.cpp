// Internal JSON helpers for the storage files (see store_json.h).
#include "net/store_json.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ks::net::detail {

namespace {

InputKind inputKindFrom(const std::string& s) {
    for (auto k : {InputKind::Url, InputKind::Wikipedia, InputKind::Html, InputKind::Text, InputKind::Markdown,
                   InputKind::Bookmarklet})
        if (ks::toString(k) == s) return k;
    return InputKind::Html;
}

} // namespace

std::recursive_mutex& storeMutex() {
    static std::recursive_mutex m;
    return m;
}

json readJson(const std::string& path) {
    bool ok = false;
    std::string text = readFile(path, &ok);
    if (!ok) return json();
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? json() : j;
}

void writeJson(const std::string& path, const json& j) {
    if (!writeFile(path, j.dump(2))) throw std::runtime_error("Could not write " + path);
}

std::vector<std::string> getStrings(const json& j, const char* key, std::vector<std::string> def) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_array()) return def;
    std::vector<std::string> out;
    for (auto& v : j[key])
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

json articleToJson(const Article& a) {
    json toc = json::array();
    for (auto& t : a.toc) toc.push_back({{"id", t.id}, {"level", t.level}, {"text", t.text}});
    json imgs = json::array();
    for (auto& i : a.images) imgs.push_back({{"src", i.src}, {"alt", i.alt}});
    return {{"id", a.id},
            {"title", a.title},
            {"author", a.author},
            {"publishedAt", a.publishedAt},
            {"capturedAt", a.capturedAt},
            {"sourceUrl", a.sourceUrl},
            {"siteName", a.siteName},
            {"excerpt", a.excerpt},
            {"language", a.language},
            {"tags", a.tags},
            {"bodyHtml", a.bodyHtml},
            {"toc", toc},
            {"images", imgs},
            {"wordCount", a.wordCount},
            {"inputKind", ks::toString(a.inputKind)}};
}

Article articleFromJson(const json& j) {
    Article a;
    a.id = get<std::string>(j, "id", "");
    a.title = get<std::string>(j, "title", "");
    a.author = get<std::string>(j, "author", "");
    a.publishedAt = get<std::string>(j, "publishedAt", "");
    a.capturedAt = get<std::string>(j, "capturedAt", "");
    a.sourceUrl = get<std::string>(j, "sourceUrl", "");
    a.siteName = get<std::string>(j, "siteName", "");
    a.excerpt = get<std::string>(j, "excerpt", "");
    a.language = get<std::string>(j, "language", "en");
    a.tags = getStrings(j, "tags");
    a.bodyHtml = get<std::string>(j, "bodyHtml", "");
    if (j.contains("toc") && j["toc"].is_array())
        for (auto& t : j["toc"]) {
            TocEntry e;
            e.id = get<std::string>(t, "id", "");
            e.level = get<int>(t, "level", 1);
            e.text = get<std::string>(t, "text", "");
            a.toc.push_back(e);
        }
    if (j.contains("images") && j["images"].is_array())
        for (auto& i : j["images"]) {
            ArticleImage im;
            im.src = get<std::string>(i, "src", "");
            im.alt = get<std::string>(i, "alt", "");
            a.images.push_back(im);
        }
    a.wordCount = get<int>(j, "wordCount", 0);
    a.inputKind = inputKindFrom(get<std::string>(j, "inputKind", "html"));
    return a;
}

json transferToJson(const TransferRecord& r) {
    return {{"id", r.id},           {"articleId", r.articleId}, {"deviceId", r.deviceId},
            {"deviceName", r.deviceName}, {"method", ks::toString(r.method)}, {"format", ks::toString(r.format)},
            {"fileName", r.fileName}, {"bytes", r.bytes},       {"ok", r.ok},
            {"error", r.error},     {"at", r.at}};
}

TransferRecord transferFromJson(const json& j) {
    TransferRecord r;
    r.id = get<std::string>(j, "id", "");
    r.articleId = get<std::string>(j, "articleId", "");
    r.deviceId = get<std::string>(j, "deviceId", "");
    r.deviceName = get<std::string>(j, "deviceName", "");
    r.method = transferMethodFrom(get<std::string>(j, "method", "")).value_or(TransferMethod::WebDav);
    r.format = outputFormatFrom(get<std::string>(j, "format", "")).value_or(OutputFormat::Epub);
    r.fileName = get<std::string>(j, "fileName", "");
    r.bytes = get<uint64_t>(j, "bytes", 0);
    r.ok = get<bool>(j, "ok", false);
    r.error = get<std::string>(j, "error", "");
    r.at = get<std::string>(j, "at", "");
    return r;
}

json limitsToJson(const PackLimits& l) {
    return {{"depth", l.depth},
            {"maxArticles", l.maxArticles},
            {"maxImagesPerArticle", l.maxImagesPerArticle},
            {"maxTotalEpubSizeMB", l.maxTotalEpubSizeMB},
            {"requestDelayMs", l.requestDelayMs},
            {"includeReferences", l.includeReferences},
            {"includeInfobox", l.includeInfobox},
            {"sameDomainOnly", l.sameDomainOnly}};
}

PackLimits packLimitsFromJson(const json& j) {
    PackLimits d;
    PackLimits l;
    l.depth = std::clamp(get<int>(j, "depth", d.depth), 0, 2);
    l.maxArticles = std::clamp(get<int>(j, "maxArticles", d.maxArticles), 1, 500);
    l.maxImagesPerArticle = std::clamp(get<int>(j, "maxImagesPerArticle", d.maxImagesPerArticle), 0, 50);
    l.maxTotalEpubSizeMB = std::clamp(get<int>(j, "maxTotalEpubSizeMB", d.maxTotalEpubSizeMB), 1, 500);
    l.requestDelayMs = std::clamp(get<int>(j, "requestDelayMs", d.requestDelayMs), 0, 10000);
    l.includeReferences = get<bool>(j, "includeReferences", d.includeReferences);
    l.includeInfobox = get<bool>(j, "includeInfobox", d.includeInfobox);
    l.sameDomainOnly = get<bool>(j, "sameDomainOnly", d.sameDomainOnly);
    return l;
}

bool safeId(const std::string& id) {
    if (id.empty()) return false;
    for (unsigned char c : id)
        if (!(std::isalnum(c) || c == '-' || c == '_')) return false;
    return true;
}

void writeImageDir(const std::string& dir, const ImageMap& images) {
    std::error_code ec;
    fs::remove_all(fs::u8path(dir), ec);
    if (images.empty()) return;
    makeDirs(dir);
    json index = json::object();
    int n = 0;
    for (auto& [src, bytes] : images) {
        char name[32];
        std::snprintf(name, sizeof name, "%03d.bin", ++n);
        if (!writeFile(joinPath(dir, name), bytes)) throw std::runtime_error("Could not write an image file in " + dir);
        index[src] = name;
    }
    writeJson(joinPath(dir, "index.json"), index);
}

ImageMap readImageDir(const std::string& dir) {
    ImageMap out;
    json index = readJson(joinPath(dir, "index.json"));
    if (!index.is_object()) return out;
    for (auto& [src, file] : index.items()) {
        if (!file.is_string()) continue;
        std::string name = file.get<std::string>();
        if (name.find_first_of("/\\") != std::string::npos || name.find("..") != std::string::npos) continue;
        bool ok = false;
        std::string bytes = readFile(joinPath(dir, name), &ok);
        if (ok) out[src] = std::move(bytes);
    }
    return out;
}

} // namespace ks::net::detail
