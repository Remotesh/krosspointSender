// K-rosspointSender desktop: extract module, extractArticle and the shared
// last step (TOC ids, image list, word count). Port of
// src/extractors/article.ts, finish.ts, toc.ts and bookmarklet.ts.
#include "extract/internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace ks::extract {

namespace detail {

// Give every heading an id ("sec-1", ...) and list the headings.
// Internal links to the old ids follow the new ids.
static std::vector<TocEntry> buildToc(Node body) {
    std::vector<TocEntry> toc;
    std::map<std::string, std::string> renamed;
    int n = 0;
    for (Node h : querySelectorAll(body, "h1, h2, h3, h4, h5, h6")) {
        ++n;
        std::string id = "sec-" + std::to_string(n);
        std::string old = attr(h, "id");
        if (!old.empty() && old != id) renamed[old] = id;
        // An anchor inside the heading can carry the target id.
        for (Node inner : querySelectorAll(h, "[id]")) {
            renamed[attr(inner, "id")] = id;
            removeAttr(inner, "id");
        }
        setAttr(h, "id", id);
        std::string text = normText(textContent(h));
        if (!text.empty()) toc.push_back(TocEntry{id, headingLevelOfTag(h), text});
    }
    // Other elements must not reuse a sec-N id. Ids must be unique and have no spaces.
    std::set<std::string> seen;
    for (Node el : descendants(body)) {
        if (!hasAttr(el, "id")) continue;
        std::string id = attr(el, "id");
        bool bad = id.empty() || seen.count(id) || (id.rfind("sec-", 0) == 0 && !isHeading(el));
        for (char c : id)
            if (std::isspace(static_cast<unsigned char>(c))) bad = true;
        if (bad) removeAttr(el, "id");
        else seen.insert(id);
    }
    if (!renamed.empty()) {
        for (Node a : querySelectorAll(body, "a[href^=\"#\"]")) {
            std::string target = percentDecode(attr(a, "href").substr(1));
            auto it = renamed.find(target);
            if (it != renamed.end()) setAttr(a, "href", "#" + it->second);
        }
    }
    return toc;
}

// List the images of a body in order. Each src is listed once.
static std::vector<ArticleImage> collectImages(Node body) {
    std::vector<ArticleImage> out;
    std::set<std::string> seen;
    for (Node img : querySelectorAll(body, "img")) {
        std::string src = attr(img, "src");
        if (src.empty() || seen.count(src)) continue;
        seen.insert(src);
        out.push_back(ArticleImage{src, attr(img, "alt")});
    }
    return out;
}

Article finishArticle(const ArticleParts& p) {
    auto dom = parseFragment(p.bodyHtml);
    Node body = dom->body();
    Article a;
    a.toc = buildToc(body);
    a.images = collectImages(body);
    a.wordCount = countWordsNode(body);
    a.bodyHtml = trim(serializeXhtml(body));
    a.id = newUuid();
    a.title = trim(p.title).empty() ? "Untitled" : trim(p.title);
    a.capturedAt = p.capturedAt.empty() ? nowIso() : p.capturedAt;
    a.language = trim(p.language).empty() ? "en" : trim(p.language);
    a.tags = p.tags;
    a.inputKind = p.inputKind;
    a.author = p.author;
    a.publishedAt = p.publishedAt;
    a.sourceUrl = p.sourceUrl;
    a.siteName = p.siteName;
    a.excerpt = p.excerpt;
    return a;
}

} // namespace detail

using namespace detail;

namespace {

std::string fileBaseName(const std::string& fileName) {
    if (fileName.empty()) return "";
    size_t slash = fileName.find_last_of("/\\");
    std::string base = slash == std::string::npos ? fileName : fileName.substr(slash + 1);
    static const std::regex ext(R"(\.[a-z0-9]{1,8}$)", std::regex::icase);
    base = std::regex_replace(base, ext, "");
    for (char& c : base)
        if (c == '_') c = ' ';
    return normText(base);
}

std::string firstHeading(const std::string& html) {
    auto dom = parseFragment(html);
    Node h = querySelector(dom->body(), "h1, h2, h3, h4, h5, h6");
    return h ? normText(textContent(h)) : "";
}

std::string firstLine(const std::string& text) {
    std::stringstream ss(text);
    std::string line;
    static const std::regex hashes(R"(^#+\s*)");
    while (std::getline(ss, line)) {
        std::string l = trim(std::regex_replace(line, hashes, ""));
        if (l.empty()) continue;
        if (u8len(l) > 120) return rtrimSpace(u8prefix(l, 117)) + "...";
        return l;
    }
    return "";
}

std::string hostName(const std::string& url) {
    std::string h = hostOf(url);
    if (h.rfind("www.", 0) == 0) h = h.substr(4);
    return h;
}

std::string firstOf(std::initializer_list<std::string> v) {
    for (const auto& s : v)
        if (!s.empty()) return s;
    return "";
}

struct HtmlCtx {
    std::string html;
    std::string sourceUrl;
    InputKind kind = InputKind::Html;
    std::string capturedAt;
    std::string fileName;
    std::string titleHint;
};

// The first paragraph text of an HTML fragment, cut to 280 characters.
std::string firstParagraph(const std::string& html) {
    auto dom = parseFragment(html);
    for (Node p : querySelectorAll(dom->body(), "p")) {
        std::string t = normText(textContent(p));
        if (t.empty()) continue;
        return u8len(t) > 280 ? rtrimSpace(u8prefix(t, 277)) + "..." : t;
    }
    return "";
}

Article extractFromHtml(const HtmlCtx& ctx, const ExtractOptions& opts) {
    auto dom = parseDocument(ctx.html);
    // Metadata first. The steps below change the document.
    PageMeta meta = readMeta(*dom);
    JsonLd ld = readJsonLd(*dom);
    std::string htmlTitle;
    if (Node t = querySelector(dom->root(), "title")) htmlTitle = normText(textContent(t));
    // A <base href> changes the base of relative links.
    std::string base = ctx.sourceUrl;
    if (Node b = querySelector(dom->root(), "base[href]")) {
        std::string href = trim(attr(b, "href"));
        if (!href.empty()) base = base.empty() ? (parseAbsUrl(href) ? href : base) : resolveUrl(href, base);
    }

    Node body = dom->body();
    removeJunk(*dom, body, true);
    fixImages(*dom, body);
    normalizeFigures(*dom, body);

    size_t bodyText = u8len(normText(textContent(body)));
    ReaderResult reader = readability(*dom, firstOf({ld.headline, meta.title, htmlTitle}));
    size_t readerText = u8len(reader.textContent);
    std::string content;
    if (reader.ok && static_cast<double>(readerText) >= std::min(200.0, static_cast<double>(bodyText) * 0.5)) content = reader.contentHtml;
    else content = innerHtml(body);

    std::string bodyHtml = sanitizeHtml(content, opts, base);
    std::string byline = normText(reader.byline);
    if (startsWithNoCase(byline, "by ")) byline = trim(byline.substr(3));

    ArticleParts parts;
    parts.bodyHtml = bodyHtml;
    parts.inputKind = ctx.kind;
    parts.title = firstOf({ld.headline, normText(reader.title), ctx.titleHint, meta.title, htmlTitle});
    if (parts.title.empty()) parts.title = firstOf({firstHeading(bodyHtml), fileBaseName(ctx.fileName), "Untitled"});
    parts.author = firstOf({meta.author, !byline.empty() && u8len(byline) < 120 ? byline : std::string()});
    parts.publishedAt = meta.publishedAt;
    parts.capturedAt = ctx.capturedAt;
    parts.sourceUrl = ctx.sourceUrl;
    parts.siteName = firstOf({meta.siteName, hostName(ctx.sourceUrl)});
    parts.excerpt = firstOf({meta.excerpt, firstParagraph(bodyHtml)});
    parts.language = firstOf({meta.language, "en"});
    parts.tags = meta.tags;
    return finishArticle(parts);
}

struct Payload {
    std::string url, html, capturedAt, title;
};

std::string decodeBase64(const std::string& in) {
    std::string s;
    for (char c : in) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        s += c == '-' ? '+' : c == '_' ? '/' : c;
    }
    static const std::regex b64(R"(^[A-Za-z0-9+/]*={0,2}$)");
    if (!std::regex_match(s, b64)) throw std::runtime_error("The payload is not JSON and not base64.");
    while (s.size() % 4) s += '=';
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i + 3 < s.size(); i += 4) {
        int a = val(s[i]), b = val(s[i + 1]), c = val(s[i + 2]), d = val(s[i + 3]);
        if (a < 0 || b < 0) break;
        out += static_cast<char>((a << 2) | (b >> 4));
        if (c >= 0) out += static_cast<char>(((b & 15) << 4) | (c >> 2));
        if (c >= 0 && d >= 0) out += static_cast<char>(((c & 3) << 6) | d);
    }
    return out;
}

// Parse a bookmarklet payload from JSON or base64 JSON. Throws on bad input.
Payload parseBookmarkletPayload(const std::string& raw) {
    std::string s = trim(raw);
    if (s.empty()) throw std::runtime_error("The payload is empty.");
    std::string text;
    if (s[0] == '{') {
        text = s;
    } else {
        try {
            text = decodeBase64(s);
        } catch (const std::exception& e) {
            throw std::runtime_error(std::string("The payload cannot be decoded: ") + e.what());
        }
    }
    nlohmann::json o = nlohmann::json::parse(text, nullptr, false);
    if (o.is_discarded()) throw std::runtime_error("The payload is not valid JSON.");
    if (!o.is_object()) throw std::runtime_error("The payload must be a JSON object.");
    if (o.contains("v") && !(o["v"].is_number() && o["v"].get<double>() == 1)) throw std::runtime_error("Unknown payload version.");
    if (!o.contains("html") || !o["html"].is_string() || trim(o["html"].get<std::string>()).empty())
        throw std::runtime_error("The payload has no HTML.");
    if (o.contains("url") && !o["url"].is_string()) throw std::runtime_error("The payload URL must be a string.");
    Payload p;
    p.html = o["html"].get<std::string>();
    p.url = o.contains("url") ? o["url"].get<std::string>() : "";
    if (!p.url.empty() && !startsWithNoCase(p.url, "http://") && !startsWithNoCase(p.url, "https://"))
        throw std::runtime_error("The payload URL must be http or https.");
    if (o.contains("capturedAt") && o["capturedAt"].is_string() && !toIsoDate(o["capturedAt"].get<std::string>()).empty())
        p.capturedAt = o["capturedAt"].get<std::string>();
    if (p.capturedAt.empty()) p.capturedAt = nowIso();
    if (o.contains("title") && o["title"].is_string()) p.title = trim(o["title"].get<std::string>());
    return p;
}

} // namespace

Article extractArticle(const RawInput& in, const ExtractOptions& opts) {
    const std::string& content = in.content;
    std::string sourceUrl = in.sourceUrl.value_or("");
    std::string fileName = in.fileName.value_or("");
    switch (in.kind) {
        case InputKind::Wikipedia:
            if (trim(content).empty()) throw std::runtime_error("No HTML to extract. Fetch the page first.");
            return extractWikipedia(content, sourceUrl, opts);

        case InputKind::Markdown: {
            std::string html = sanitizeHtml(markdownToHtml(content), opts, sourceUrl);
            ArticleParts p;
            p.bodyHtml = html;
            p.inputKind = InputKind::Markdown;
            p.title = firstOf({firstHeading(html), firstLine(content), fileBaseName(fileName), "Untitled"});
            p.sourceUrl = sourceUrl;
            return finishArticle(p);
        }

        case InputKind::Text: {
            ArticleParts p;
            p.bodyHtml = textToHtml(content);
            p.inputKind = InputKind::Text;
            p.title = firstOf({firstLine(content), fileBaseName(fileName), "Untitled"});
            p.sourceUrl = sourceUrl;
            return finishArticle(p);
        }

        case InputKind::Bookmarklet: {
            HtmlCtx ctx;
            ctx.html = content;
            ctx.sourceUrl = sourceUrl;
            ctx.kind = InputKind::Bookmarklet;
            ctx.fileName = fileName;
            if (trim(content).rfind('<', 0) != 0) {
                Payload p = parseBookmarkletPayload(content);
                ctx.html = p.html;
                if (!p.url.empty()) ctx.sourceUrl = p.url;
                ctx.capturedAt = p.capturedAt;
                ctx.titleHint = p.title;
            }
            if (!ctx.sourceUrl.empty() && isWikipediaUrl(ctx.sourceUrl)) {
                Article a = extractWikipedia(ctx.html, ctx.sourceUrl, opts);
                a.inputKind = InputKind::Bookmarklet;
                if (!ctx.capturedAt.empty()) a.capturedAt = ctx.capturedAt;
                return a;
            }
            return extractFromHtml(ctx, opts);
        }

        case InputKind::Url:
        case InputKind::Html:
        default: {
            if (trim(content).empty())
                throw std::runtime_error(in.kind == InputKind::Url ? "No HTML to extract. Fetch the page first." : "The HTML is empty.");
            if (!sourceUrl.empty() && isWikipediaUrl(sourceUrl)) {
                // A Wikipedia page (fetched or pasted). Use the Wikipedia clean-up path.
                Article a = extractWikipedia(content, sourceUrl, opts);
                a.inputKind = in.kind;
                return a;
            }
            HtmlCtx ctx;
            ctx.html = content;
            ctx.sourceUrl = sourceUrl;
            ctx.kind = in.kind;
            ctx.fileName = fileName;
            return extractFromHtml(ctx, opts);
        }
    }
}

} // namespace ks::extract
