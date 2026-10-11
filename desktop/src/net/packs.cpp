// Spider pack storage: dataDir()/packs/<id>/pack.json + images/.
// pack.json holds every field of Pack (the pages with their articles too)
// and the library data of the pack (last file name, sends).
// Writes are atomic (ks::writeFile). Bad files are skipped. Reads never throw.
#include "net/store_json.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace ks::net {

using namespace detail;

namespace {

json candidateToJson(const PackCandidate& c) {
    return {{"key", c.key},
            {"title", c.title},
            {"url", c.url},
            {"depth", c.depth},
            {"parentKey", c.parentKey},
            {"linkCount", c.linkCount},
            {"selected", c.selected},
            {"disambiguation", c.disambiguation},
            {"redirect", c.redirect},
            {"manual", c.manual}};
}

PackCandidate candidateFromJson(const json& j) {
    PackCandidate c;
    c.key = get<std::string>(j, "key", "");
    c.title = get<std::string>(j, "title", "");
    c.url = get<std::string>(j, "url", "");
    c.depth = get<int>(j, "depth", 0);
    c.parentKey = get<std::string>(j, "parentKey", "");
    c.linkCount = get<int>(j, "linkCount", 0);
    c.selected = get<bool>(j, "selected", false);
    c.disambiguation = get<bool>(j, "disambiguation", false);
    c.redirect = get<bool>(j, "redirect", false);
    c.manual = get<bool>(j, "manual", false);
    return c;
}

json pageToJson(const PackPage& p) {
    json j = {{"candidate", candidateToJson(p.candidate)},
              {"state", ks::toString(p.state)},
              {"error", p.error},
              {"fileName", p.fileName}};
    j["article"] = p.article ? articleToJson(*p.article) : json();
    return j;
}

PackPage pageFromJson(const json& j) {
    PackPage p;
    if (j.contains("candidate")) p.candidate = candidateFromJson(j["candidate"]);
    p.state = packStateFrom(get<std::string>(j, "state", "")).value_or(PackPageState::Queued);
    if (j.contains("article") && j["article"].is_object()) p.article = articleFromJson(j["article"]);
    p.error = get<std::string>(j, "error", "");
    p.fileName = get<std::string>(j, "fileName", "");
    return p;
}

json packToJson(const Pack& p) {
    json pages = json::array();
    for (auto& pg : p.pages) pages.push_back(pageToJson(pg));
    json ext = json::array();
    for (auto& e : p.externalLinks)
        ext.push_back({{"key", e.key}, {"title", e.title}, {"url", e.url}, {"fromKey", e.fromKey}});
    return {{"id", p.id},
            {"kind", ks::toString(p.kind)},
            {"title", p.title},
            {"author", p.author},
            {"rootKey", p.rootKey},
            {"rootUrl", p.rootUrl},
            {"limits", limitsToJson(p.limits)},
            {"pages", pages},
            {"externalLinks", ext},
            {"createdAt", p.createdAt},
            {"updatedAt", p.updatedAt}};
}

Pack packFromJson(const json& j) {
    Pack p;
    p.id = get<std::string>(j, "id", "");
    p.kind = packKindFrom(get<std::string>(j, "kind", "")).value_or(PackSourceKind::Wikipedia);
    p.title = get<std::string>(j, "title", "");
    p.author = get<std::string>(j, "author", "");
    p.rootKey = get<std::string>(j, "rootKey", "");
    p.rootUrl = get<std::string>(j, "rootUrl", "");
    p.limits = packLimitsFromJson(j.contains("limits") ? j["limits"] : json::object());
    if (j.contains("pages") && j["pages"].is_array())
        for (auto& pg : j["pages"])
            if (pg.is_object()) p.pages.push_back(pageFromJson(pg));
    if (j.contains("externalLinks") && j["externalLinks"].is_array())
        for (auto& e : j["externalLinks"]) {
            PackExternalLink l;
            l.key = get<std::string>(e, "key", "");
            l.title = get<std::string>(e, "title", "");
            l.url = get<std::string>(e, "url", "");
            l.fromKey = get<std::string>(e, "fromKey", "");
            p.externalLinks.push_back(l);
        }
    p.createdAt = get<std::string>(j, "createdAt", "");
    p.updatedAt = get<std::string>(j, "updatedAt", "");
    return p;
}

std::string packsDir() { return joinPath(dataDir(), "packs"); }
std::string packDir(const std::string& id) { return joinPath(packsDir(), id); }
std::string packPath(const std::string& id) { return joinPath(packDir(id), "pack.json"); }

// The whole file: {"pack": {...}, "fileName": "...", "transfers": [...]}.
json readPackFile(const std::string& id) {
    if (!safeId(id)) return json();
    json j = readJson(packPath(id));
    return j.is_object() && j.contains("pack") && j["pack"].is_object() ? j : json();
}

} // namespace

void savePack(const Pack& in, const ImageMap* images) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    if (in.id.empty()) throw std::runtime_error("The pack has no id.");
    if (!safeId(in.id)) throw std::runtime_error("The pack id contains characters that are not allowed: " + in.id);
    Pack p = in;
    const std::string now = nowIso();
    json old = readPackFile(p.id);
    if (p.createdAt.empty()) p.createdAt = old.is_object() ? get<std::string>(old["pack"], "createdAt", now) : now;
    if (p.createdAt.empty()) p.createdAt = now;
    if (p.updatedAt.empty()) p.updatedAt = now;
    makeDirs(packDir(p.id));
    if (images) writeImageDir(joinPath(packDir(p.id), "images"), *images);
    json file = {{"version", 1}, {"pack", packToJson(p)}};
    file["fileName"] = old.is_object() ? get<std::string>(old, "fileName", "") : "";
    file["transfers"] = old.is_object() && old.contains("transfers") ? old["transfers"] : json::array();
    writeJson(packPath(p.id), file);
}

std::optional<Pack> getPack(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    json j = readPackFile(id);
    if (!j.is_object()) return std::nullopt;
    return packFromJson(j["pack"]);
}

std::vector<Pack> listPacks() {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    std::vector<Pack> out;
    std::error_code ec;
    fs::path dir = fs::u8path(packsDir());
    if (!fs::is_directory(dir, ec)) return out;
    for (auto& d : fs::directory_iterator(dir, ec)) {
        if (!d.is_directory(ec)) continue;
        json j = readPackFile(d.path().filename().u8string());
        if (j.is_object()) out.push_back(packFromJson(j["pack"]));
    }
    std::sort(out.begin(), out.end(), [](const Pack& a, const Pack& b) {
        if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
        return a.createdAt > b.createdAt;
    });
    return out;
}

ImageMap loadPackImages(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    if (!safeId(id)) return {};
    return readImageDir(joinPath(packDir(id), "images"));
}

void deletePack(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    if (!safeId(id)) return;
    std::error_code ec;
    fs::remove_all(fs::u8path(packDir(id)), ec);
}

PackRecord getPackRecord(const std::string& id) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    PackRecord r;
    json j = readPackFile(id);
    if (!j.is_object()) return r;
    r.fileName = get<std::string>(j, "fileName", "");
    if (j.contains("transfers") && j["transfers"].is_array())
        for (auto& t : j["transfers"]) r.transfers.push_back(transferFromJson(t));
    return r;
}

void updatePackRecord(const std::string& id, const std::string& fileName, const std::optional<TransferRecord>& rec) {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    json j = readPackFile(id);
    if (!j.is_object()) return;
    if (!fileName.empty()) j["fileName"] = fileName;
    if (rec) {
        TransferRecord t = *rec;
        if (t.id.empty()) t.id = newUuid();
        if (t.at.empty()) t.at = nowIso();
        t.articleId = id;
        if (!j.contains("transfers") || !j["transfers"].is_array()) j["transfers"] = json::array();
        j["transfers"].push_back(transferToJson(t));
    }
    j["pack"]["updatedAt"] = nowIso();
    writeJson(packPath(id), j);
}

namespace detail {

void clearPacks() {
    std::lock_guard<std::recursive_mutex> lk(storeMutex());
    std::error_code ec;
    fs::remove_all(fs::u8path(packsDir()), ec);
}

} // namespace detail

} // namespace ks::net
