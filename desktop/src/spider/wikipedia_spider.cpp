// k-rosspointSender desktop: spider module, Wikipedia link scan.
// Port of src/spider/wikipediaSpider.ts.
#include "spider/internal.h"
#include "extract/internal.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

namespace ks::spider {

namespace ex = ks::extract::detail;
using namespace detail;

namespace {

// Figure and caption containers. Their links come after the body-text links.
const char* kCaptionSelector =
    "figure, figcaption, .thumb, .thumbinner, .thumbcaption, .gallery, .gallerybox, .gallerytext, .tsingle, .multiimageinner";

// Titles per pageprops query (the API limit for normal users).
const size_t kPagePropsBatch = 50;

ex::Node closestMatch(ex::Node n, const ex::Selector& sel) {
    for (ex::Node p = n; p && ex::isElement(p); p = ex::parentElement(p))
        if (sel.matches(p)) return p;
    return nullptr;
}

struct ScanResult {
    std::string title;
    std::vector<std::string> links;
};

// Fetch one page and return its canonical title and its body links.
ScanResult scanPage(const std::string& lang, const std::string& title, const bool* cancel, const OnWait& onWait) {
    nlohmann::json data = wikiApi(
        lang, {{"action", "parse"}, {"page", title}, {"prop", "text|links"}, {"redirects", "1"}, {"disableeditsection", "1"}}, cancel,
        onWait);
    if (!data.contains("parse") || !data["parse"].is_object() || !data["parse"].contains("text") || !data["parse"]["text"].is_string())
        throw std::runtime_error("Wikipedia has no text for \"" + title + "\".");
    const auto& p = data["parse"];
    std::string canon = canonicalTitle(p.contains("title") && p["title"].is_string() && !p["title"].get<std::string>().empty()
                                           ? p["title"].get<std::string>()
                                           : title);
    std::vector<LinkMeta> meta;
    if (p.contains("links") && p["links"].is_array()) {
        for (const auto& l : p["links"]) {
            if (!l.is_object() || !l.contains("title") || !l["title"].is_string()) continue;
            LinkMeta m;
            m.title = l["title"].get<std::string>();
            m.ns = l.contains("ns") && l["ns"].is_number_integer() ? l["ns"].get<int>() : 0;
            m.exists = !(l.contains("exists") && l["exists"].is_boolean() && !l["exists"].get<bool>());
            meta.push_back(m);
        }
    }
    return {canon, extractBodyLinks(p["text"].get<std::string>(), lang, canon, meta)};
}

struct Entry {
    PackCandidate cand;
    std::vector<std::string> linkers;   // insertion order, unique
    void addLinker(const std::string& l) {
        if (std::find(linkers.begin(), linkers.end(), l) == linkers.end()) linkers.push_back(l);
    }
};

} // namespace

std::optional<WikiRoot> normalizeWikipediaTitle(const std::string& input, const std::string& lang) {
    std::string s = trim(input);
    if (s.empty()) return std::nullopt;
    std::string outLang = toLower(lang.empty() ? std::string("en") : lang);
    std::string title;
    static const std::regex scheme(R"(^https?://)", std::regex::icase);
    static const std::regex wikiHost(R"((^|\.)wikipedia\.org(/|$))", std::regex::icase);
    bool looksLikeUrl = std::regex_search(s, scheme) || s.rfind("//", 0) == 0 || std::regex_search(s, wikiHost);
    if (looksLikeUrl) {
        std::string abs = std::regex_search(s, scheme) ? s : "https://" + (s.rfind("//", 0) == 0 ? s.substr(2) : s);
        auto p = extract::parseWikipediaUrl(abs);
        if (!p) return std::nullopt;
        outLang = p->lang;
        title = canonicalTitle(p->title);
    } else {
        title = canonicalTitle(s.substr(0, s.find('#')));
    }
    if (title.empty() || hasNonArticleNamespace(title)) return std::nullopt;
    return WikiRoot{outLang, title, wikipediaArticleUrl(outLang, title)};
}

std::vector<std::string> extractBodyLinks(const std::string& html, const std::string& lang, const std::string& selfTitle,
                                          const std::vector<LinkMeta>& linksMeta) {
    std::map<std::string, LinkMeta> meta;
    for (const auto& l : linksMeta) meta[canonicalTitle(l.title)] = l;
    auto dom = ex::parseFragment(html);
    ex::Node body = dom->body();
    ex::Node root = ex::querySelector(body, ".mw-parser-output");
    if (!root) root = body;
    const ex::Selector nonBody(kNonBodySelector);
    const ex::Selector caption(kCaptionSelector);
    const std::string self = canonicalTitle(selfTitle);
    std::set<std::string> seen;
    std::vector<std::string> out, captionLinks;
    for (ex::Node a : ex::querySelectorAll(root, "a[href]")) {
        if (ex::hasClass(a, "new") || ex::hasClass(a, "extiw") || ex::hasClass(a, "external")) continue;
        if (closestMatch(a, nonBody)) continue;
        WikiHref c = classifyWikiHref(ex::attr(a, "href"), lang);
        if (c.type != WikiHref::Article || c.lang != lang) continue;
        if (c.title == self || seen.count(c.title)) continue;
        auto m = meta.find(c.title);
        if (m != meta.end() && (m->second.ns != 0 || !m->second.exists)) continue;
        if (closestMatch(a, caption)) {
            if (std::find(captionLinks.begin(), captionLinks.end(), c.title) == captionLinks.end()) captionLinks.push_back(c.title);
            continue;
        }
        seen.insert(c.title);
        out.push_back(c.title);
    }
    // Ranking: body-text links first, then links that occur only in figures and captions.
    for (const auto& t : captionLinks)
        if (!seen.count(t)) out.push_back(t);
    return out;
}

std::vector<PackCandidate> scanWikipediaLinks(const WikiRoot& root, const PackLimits& limits, const PackProgressFn& onProgress,
                                              const bool* cancel) {
    const std::string lang = toLower(root.lang.empty() ? std::string("en") : root.lang);
    const size_t cap = static_cast<size_t>(std::max(1, limits.maxArticles * 5));
    Pacer pacer(limits.requestDelayMs, cancel);
    std::vector<PackError> errors;
    PackProgress last;
    last.phase = PackPhase::Scanning;
    last.total = 1;
    auto send = [&](const PackProgress& p) {
        if (!onProgress) return;
        try {
            onProgress(p);
        } catch (...) {
            // A failing listener must not stop the scan.
        }
    };
    auto emit = [&](int done, int total, const std::string& current, const std::string& message) {
        last.phase = PackPhase::Scanning;
        last.done = done;
        last.total = total;
        last.currentTitle = current;
        last.message = message;
        last.errors = errors;
        send(last);
    };
    OnWait onWait = [&](int s) {
        PackProgress p = last;
        p.message = "rate limited, waiting " + std::to_string(s) + "s";
        p.errors = errors;
        send(p);
    };

    std::string rootTitle = canonicalTitle(root.title);
    bool rootRedirect = false;
    std::vector<Entry> entries;
    std::map<std::string, size_t> index;
    auto add = [&](const std::string& title, int depth, const std::string& parentKey, const std::string& linker) {
        auto it = index.find(title);
        if (it != index.end()) {
            if (!linker.empty() && linker != title) entries[it->second].addLinker(linker);
            return;
        }
        if (entries.size() >= cap) return;
        Entry e;
        e.cand.key = title;
        e.cand.title = title;
        e.cand.url = wikipediaArticleUrl(lang, title);
        e.cand.depth = depth;
        e.cand.parentKey = parentKey;
        if (!linker.empty()) e.linkers.push_back(linker);
        index[title] = entries.size();
        entries.push_back(std::move(e));
    };

    try {
        throwIfCancelled(cancel);
        if (limits.depth <= 0) {
            add(rootTitle, 0, "", "");
            entries[0].cand.selected = true;
            emit(1, 1, rootTitle, "Depth 0: only the root page.");
            return {entries[0].cand};
        }

        // Depth 0 -> 1: the root page.
        emit(0, 1, rootTitle, "Reading the root article.");
        pacer.wait();
        ScanResult first = scanPage(lang, rootTitle, cancel, onWait);
        if (first.title != rootTitle) {
            rememberAlias(lang, rootTitle, first.title);
            rootTitle = first.title;
            rootRedirect = true;
        }
        add(rootTitle, 0, "", "");
        for (const auto& t : first.links) add(t, 1, rootTitle, rootTitle);

        // Depth 1 -> 2: scan depth-1 pages while there is room for candidates.
        std::vector<std::string> depth1;
        for (const auto& e : entries)
            if (e.cand.depth == 1) depth1.push_back(e.cand.key);
        const int total = limits.depth >= 2 ? 1 + static_cast<int>(depth1.size()) : 1;
        int done = 1;
        emit(done, total, rootTitle, "Found " + std::to_string(entries.size() - 1) + " linked articles.");
        if (limits.depth >= 2) {
            for (const auto& key : depth1) {
                if (entries.size() >= cap) break;
                throwIfCancelled(cancel);
                emit(done, total, key, "");
                try {
                    pacer.wait();
                    ScanResult res = scanPage(lang, key, cancel, onWait);
                    if (res.title != key) rememberAlias(lang, key, res.title);
                    for (const auto& t : res.links)
                        if (t != key) add(t, 2, key, key);
                } catch (const CancelledError&) {
                    throw;
                } catch (const std::exception& e) {
                    errors.push_back({key, key, e.what()});
                }
                ++done;
            }
        }

        // Disambiguation and redirect check, 50 titles per request.
        std::vector<std::string> titles;
        for (const auto& e : entries) titles.push_back(e.cand.key);
        std::map<std::string, std::string> finalTitle;   // asked title -> canonical title
        std::set<std::string> disambig, missing;
        for (size_t i = 0; i < titles.size(); i += kPagePropsBatch) {
            std::vector<std::string> batch(titles.begin() + i, titles.begin() + std::min(titles.size(), i + kPagePropsBatch));
            emit(done, total, batch[0], "Checking for disambiguation pages.");
            try {
                pacer.wait();
                std::string joined;
                for (const auto& t : batch) joined += (joined.empty() ? "" : "|") + t;
                nlohmann::json data = wikiApi(
                    lang, {{"action", "query"}, {"prop", "pageprops"}, {"ppprop", "disambiguation"}, {"redirects", "1"}, {"titles", joined}},
                    cancel, onWait);
                std::map<std::string, std::string> norm, redir;
                const nlohmann::json q = data.contains("query") && data["query"].is_object() ? data["query"] : nlohmann::json::object();
                auto readPairs = [&](const char* name, std::map<std::string, std::string>& into) {
                    if (!q.contains(name) || !q[name].is_array()) return;
                    for (const auto& r : q[name])
                        if (r.is_object() && r.contains("from") && r.contains("to") && r["from"].is_string() && r["to"].is_string())
                            into[r["from"].get<std::string>()] = r["to"].get<std::string>();
                };
                readPairs("normalized", norm);
                readPairs("redirects", redir);
                for (const auto& t : batch) {
                    std::string f = norm.count(t) ? norm[t] : t;
                    for (int hop = 0; hop < 5 && redir.count(f); ++hop) f = redir[f];
                    finalTitle[t] = canonicalTitle(f);
                }
                if (q.contains("pages") && q["pages"].is_array()) {
                    for (const auto& p : q["pages"]) {
                        if (!p.is_object() || !p.contains("title") || !p["title"].is_string()) continue;
                        std::string pt = canonicalTitle(p["title"].get<std::string>());
                        auto flag = [&](const char* k) { return p.contains(k) && !(p[k].is_boolean() && !p[k].get<bool>()); };
                        if (flag("missing") || flag("invalid")) missing.insert(pt);
                        else if (p.contains("pageprops") && p["pageprops"].is_object() && p["pageprops"].contains("disambiguation"))
                            disambig.insert(pt);
                    }
                }
            } catch (const CancelledError&) {
                throw;
            } catch (const std::exception& e) {
                errors.push_back({batch[0], "Disambiguation check", e.what()});
            }
        }

        // Merge redirects into their targets, drop missing pages. Entries are in
        // BFS order, so the first entry for a canonical title has the lowest depth.
        std::vector<Entry> merged;
        std::map<std::string, size_t> mergedIndex;
        std::map<std::string, std::string> renamed;
        for (auto& e : entries) {
            const std::string key = e.cand.key;
            const bool isRoot = key == rootTitle;
            const std::string f = finalTitle.count(key) ? finalTitle[key] : key;
            if (!isRoot && missing.count(f)) continue;
            if (f != key) {
                rememberAlias(lang, key, f);
                renamed[key] = f;
            }
            auto prev = mergedIndex.find(f);
            if (prev != mergedIndex.end()) {
                Entry& pe = merged[prev->second];
                for (const auto& l : e.linkers) pe.addLinker(l);
                // The page is also linked by its own title: it is not only a redirect.
                if (f == key && pe.cand.redirect && pe.cand.depth != 0) pe.cand.redirect = false;
                continue;
            }
            if (f != key) {
                e.cand.key = f;
                e.cand.title = f;
                e.cand.url = wikipediaArticleUrl(lang, f);
                e.cand.redirect = true;
                if (isRoot) rootTitle = f;
            }
            if (disambig.count(f)) e.cand.disambiguation = true;
            mergedIndex[f] = merged.size();
            merged.push_back(std::move(e));
        }

        std::vector<PackCandidate> out;
        for (auto& e : merged) {
            PackCandidate c = e.cand;
            if (!c.parentKey.empty() && renamed.count(c.parentKey)) c.parentKey = renamed[c.parentKey];
            std::set<std::string> linkers;
            for (const auto& l : e.linkers) linkers.insert(renamed.count(l) ? renamed[l] : l);
            linkers.erase(c.key);
            c.linkCount = static_cast<int>(linkers.size());
            if (c.depth == 0 && rootRedirect) c.redirect = true;
            out.push_back(c);
        }
        // Order: depth, then first appearance (stable inside one depth).
        std::stable_sort(out.begin(), out.end(), [](const PackCandidate& a, const PackCandidate& b) { return a.depth < b.depth; });

        // Selection: the root, then depth-1 pages that are not disambiguation pages.
        int selected = 0;
        for (auto& c : out) {
            if (c.depth == 0) {
                c.selected = true;
                ++selected;
            } else if (c.depth == 1 && selected < limits.maxArticles && !c.disambiguation) {
                c.selected = true;
                ++selected;
            } else {
                c.selected = false;
            }
        }
        emit(total, total, rootTitle, "Found " + std::to_string(out.size() - 1) + " candidates.");
        return out;
    } catch (const CancelledError&) {
        PackProgress p = last;
        p.phase = PackPhase::Cancelled;
        p.message = "Cancelled.";
        p.errors = errors;
        send(p);
        throw;
    }
}

} // namespace ks::spider
