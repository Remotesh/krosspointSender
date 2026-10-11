// K-rosspointSender desktop: spider module, link rewriting between pack pages.
// Port of src/spider/linkRewriter.ts (and limitImages from src/spider/pack.ts).
#include "spider/internal.h"
#include "extract/internal.h"

#include <algorithm>
#include <functional>
#include <map>
#include <regex>
#include <set>

namespace ks::spider {

namespace ex = ks::extract::detail;
using namespace detail;

namespace {

// File names that the EPUB builder uses for its own pages.
const char* const kReserved[] = {"index", "nav", "toc", "external-links", "missing", "attribution", "cover", "title"};

const char* kArrow = "\xE2\x86\x97";   // U+2197

// Give each page a unique file name. Existing names stay.
std::vector<PackPage> assignFileNames(const std::vector<PackPage>& pages) {
    std::set<std::string> used;
    for (const char* r : kReserved) used.insert(std::string(r) + ".xhtml");
    for (const auto& p : pages)
        if (!p.fileName.empty()) used.insert(p.fileName);
    std::vector<PackPage> out = pages;
    for (auto& p : out) {
        if (!p.fileName.empty()) continue;
        std::string base = packSlug(p.article && !p.article->title.empty() ? p.article->title : p.candidate.title);
        std::string name = base + ".xhtml";
        for (int i = 2; used.count(name); ++i) name = base + "_" + std::to_string(i) + ".xhtml";
        used.insert(name);
        p.fileName = name;
    }
    return out;
}

std::string langOfUrl(const std::string& url) {
    static const std::regex re(R"(^https?://([a-z-]+)\.(?:m\.)?wikipedia\.org)", std::regex::icase);
    std::smatch m;
    if (std::regex_search(url, m, re) && toLower(m[1].str()) != "m") return toLower(m[1].str());
    return "en";
}

// Text of a link without the arrow marker.
std::string linkText(ex::Node a) {
    std::string out;
    std::function<void(ex::Node)> walk = [&](ex::Node n) {
        for (ex::Node c = n->children; c; c = c->next) {
            if (ex::isText(c) && c->content) out += reinterpret_cast<const char*>(c->content);
            else if (ex::isElement(c) && !(ex::isTag(c, "span") && ex::hasClass(c, "ext"))) walk(c);
        }
    };
    walk(a);
    return ex::normText(out);
}

// Mark a link as external: class "ext" and an arrow marker inside the anchor.
void markExternal(ex::Dom& dom, ex::Node a) {
    if (!ex::hasClass(a, "ext")) {
        std::string cls = trim(ex::attr(a, "class"));
        ex::setAttr(a, "class", cls.empty() ? "ext" : cls + " ext");
    }
    if (ex::querySelector(a, "span.ext")) return;
    ex::Node span = dom.createElement("span");
    ex::setAttr(span, "class", "ext");
    ex::appendChild(span, dom.createText(kArrow));
    ex::appendChild(a, span);
}

} // namespace

Rewritten rewriteLinks(const std::vector<PackPage>& pages, PackSourceKind kind) {
    Rewritten result;
    std::vector<PackPage> named = assignFileNames(pages);
    const std::string lang = langOfUrl(named.empty() ? "" : named[0].candidate.url);
    std::set<std::string> fileNames;
    for (const auto& p : named) fileNames.insert(p.fileName);

    // Target key -> file name, for bundled pages only.
    std::map<std::string, std::string> bundled;
    for (const auto& p : named) {
        if (!p.article) continue;
        std::vector<std::string> keys;
        if (kind == PackSourceKind::Wikipedia) keys = {canonicalTitle(p.candidate.key), canonicalTitle(p.article->title)};
        else keys = {webKey(p.candidate.key), webKey(p.candidate.url), p.article->sourceUrl.empty() ? "" : webKey(p.article->sourceUrl)};
        for (const auto& k : keys)
            if (!k.empty() && !bundled.count(k)) bundled[k] = p.fileName;
    }

    std::set<std::string> externalKeys;
    auto addExternal = [&](const std::string& key, const std::string& title, const std::string& url, const std::string& fromKey) {
        if (!externalKeys.insert(key).second) return;
        result.external.push_back({key, title, url, fromKey});
    };
    static const std::regex script(R"(^(javascript|vbscript|data):)", std::regex::icase);

    for (auto& page : named) {
        if (!page.article) continue;
        auto dom = ex::parseFragment(page.article->bodyHtml);
        ex::Node body = dom->body();
        std::set<std::string> ids;
        for (ex::Node e : ex::descendants(body))
            if (ex::hasAttr(e, "id")) ids.insert(ex::attr(e, "id"));
        const std::string own = page.fileName;
        const std::string fromKey = page.candidate.key;
        const std::string base = !page.article->sourceUrl.empty() ? page.article->sourceUrl : page.candidate.url;

        for (ex::Node a : ex::querySelectorAll(body, "a")) {
            std::string href = trim(ex::attr(a, "href"));
            if (href.empty()) {
                dom->unwrap(a);
                continue;
            }
            if (fileNames.count(href.substr(0, href.find('#')))) continue;   // Already rewritten.
            if (href[0] == '#') {
                if (!ids.count(href.substr(1)) && !ids.count(safeDecode(href.substr(1)))) dom->unwrap(a);
                continue;
            }
            if (std::regex_search(href, script)) {
                dom->unwrap(a);
                continue;
            }
            if (kind == PackSourceKind::Wikipedia) {
                WikiHref c = classifyWikiHref(href, lang);
                if (c.type == WikiHref::Junk) {
                    dom->unwrap(a);
                } else if (c.type == WikiHref::Fragment) {
                    if (!ids.count(c.fragment)) dom->unwrap(a);
                } else if (c.type == WikiHref::Article) {
                    if (c.lang != lang) continue;   // Another language: keep the URL.
                    const std::string key = resolveAlias(lang, c.title);
                    std::string target = bundled.count(key) ? bundled[key] : bundled.count(c.title) ? bundled[c.title] : "";
                    if (!target.empty()) {
                        bool selfFrag = target == own && !c.fragment.empty() && ids.count(c.fragment);
                        ex::setAttr(a, "href", selfFrag ? "#" + c.fragment : target);
                    } else {
                        const std::string url = wikipediaArticleUrl(lang, key);
                        ex::setAttr(a, "href", url);
                        markExternal(*dom, a);
                        addExternal(key, key, url, fromKey);
                    }
                }
                // Other: a citation or reference URL on another site. Keep it.
                continue;
            }
            // Web packs.
            const std::string key = webKey(href, base);
            if (key.empty()) continue;   // mailto: and similar stay.
            auto t = bundled.find(key);
            if (t != bundled.end()) {
                ex::setAttr(a, "href", t->second);
            } else {
                markExternal(*dom, a);
                std::string text = linkText(a);
                addExternal(key, text.empty() ? key : text, key, fromKey);
            }
        }
        Article art = *page.article;
        art.bodyHtml = ex::serializeXhtml(body);
        page.article = std::move(art);
    }
    result.pages = std::move(named);
    return result;
}

namespace detail {

Article limitImages(const Article& a, int max) {
    const size_t limit = static_cast<size_t>(std::max(0, max));
    if (a.images.size() <= limit) return a;
    std::set<std::string> keep;
    for (size_t i = 0; i < limit; ++i) keep.insert(a.images[i].src);
    auto dom = ex::parseFragment(a.bodyHtml);
    ex::Node body = dom->body();
    for (ex::Node img : ex::querySelectorAll(body, "img"))
        if (!keep.count(ex::attr(img, "src"))) dom->remove(img);
    for (ex::Node fig : ex::querySelectorAll(body, "figure"))
        if (ex::isConnected(fig) && !ex::querySelector(fig, "img")) dom->remove(fig);
    Article out = a;
    out.bodyHtml = ex::serializeXhtml(body);
    out.images.resize(limit);
    return out;
}

} // namespace detail

} // namespace ks::spider
