// k-rosspointSender desktop: extract module, URL helpers and input-kind
// detection. Port of src/extractors/url.ts and parseWikipediaUrl in fetch.ts.
#include "extract/internal.h"

#include <cctype>
#include <map>
#include <regex>
#include <sstream>

namespace ks::extract {

using namespace detail;

// Query parameters that only track the user. We remove them.
static const std::set<std::string>& trackingParams() {
    static const std::set<std::string> s = {
        "fbclid", "gclid", "dclid", "gbraid", "wbraid", "msclkid", "yclid", "twclid", "igshid", "mc_cid", "mc_eid", "_hsenc", "_hsmi",
        "mkt_tok", "oly_anon_id", "oly_enc_id", "vero_id", "wickedid", "ref_src", "spm", "__s", "s_cid", "cmpid", "ncid", "sr_share"};
    return s;
}

static bool hasSpace(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        size_t len = 0;
        if (isUnicodeSpace(s, i, &len)) return true;
    }
    return false;
}

std::optional<std::string> normalizeUrl(const std::string& in) {
    std::string s = trim(in);
    if (s.empty() || hasSpace(s)) return std::nullopt;
    static const std::regex hasScheme(R"(^[a-z][a-z0-9+.-]*://)", std::regex::icase);
    static const std::regex badScheme(R"(^(javascript|data|mailto|file|about|blob|vbscript|ftp|tel):)", std::regex::icase);
    if (s.rfind("//", 0) == 0) {
        s = "https:" + s;
    } else if (!std::regex_search(s, hasScheme)) {
        // A scheme without "//" (mailto:, javascript:, data:) is not a web URL.
        if (std::regex_search(s, badScheme)) return std::nullopt;
        s = "https://" + s;
    }
    auto u = parseAbsUrl(s);
    if (!u) return std::nullopt;
    if (u->scheme != "http" && u->scheme != "https") return std::nullopt;
    if (u->host.empty()) return std::nullopt;
    // A bare word is not a host. Localhost and IPv6 literals are exceptions.
    if (u->host.find('.') == std::string::npos && u->host != "localhost" && u->host[0] != '[') return std::nullopt;
    // A trailing dot is valid DNS. Leading or double dots are not.
    if (u->host.front() == '.' || u->host.find("..") != std::string::npos) return std::nullopt;
    if (u->hasQuery) {
        std::string kept;
        std::stringstream ss(u->query);
        std::string piece;
        while (std::getline(ss, piece, '&')) {
            if (piece.empty()) continue;
            std::string key = toLower(percentDecode(piece.substr(0, piece.find('=')), true));
            if (key.rfind("utm_", 0) == 0 || trackingParams().count(key)) continue;
            kept += (kept.empty() ? "" : "&") + piece;
        }
        u->query = kept;
        u->hasQuery = !kept.empty();
    }
    return u->toString();
}

bool isWikipediaUrl(const std::string& url) {
    auto n = normalizeUrl(url);
    if (!n) return false;
    auto u = parseAbsUrl(*n);
    if (!u) return false;
    static const std::regex host(R"((^|\.)wikipedia\.org$)");
    if (!std::regex_search(u->host, host)) return false;
    return u->path.rfind("/wiki/", 0) == 0 || u->path.rfind("/w/index.php", 0) == 0;
}

std::optional<WikiRef> parseWikipediaUrl(const std::string& url) {
    auto n = normalizeUrl(url);
    if (!n) return std::nullopt;
    auto u = parseAbsUrl(*n);
    if (!u) return std::nullopt;
    static const std::regex hostRe(R"(^(?:([a-z-]+)\.)?(?:m\.)?wikipedia\.org$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(u->host, m, hostRe)) return std::nullopt;
    std::string lang = m[1].matched && toLower(m[1].str()) != "m" ? toLower(m[1].str()) : "en";
    std::string title;
    if (u->path.rfind("/wiki/", 0) == 0 && u->path.size() > 6) {
        title = u->path.substr(6);
    } else {
        std::stringstream ss(u->query);
        std::string piece;
        while (std::getline(ss, piece, '&')) {
            size_t eq = piece.find('=');
            if (eq != std::string::npos && percentDecode(piece.substr(0, eq), true) == "title") {
                title = percentDecode(piece.substr(eq + 1), true);
                break;
            }
        }
    }
    if (title.empty()) return std::nullopt;
    title = percentDecode(title);
    for (char& c : title)
        if (c == '_') c = ' ';
    title = title.substr(0, title.find('#'));
    if (trim(title).empty()) return std::nullopt;
    return WikiRef{lang, title, lang + ".wikipedia.org"};
}

// True when any line of the text matches the regex (like the JS "m" flag).
static bool anyLine(const std::string& text, const std::regex& re) {
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, re)) return true;
    }
    return false;
}

InputKind detectInputKind(const std::string& text, const std::string& fileName) {
    if (!fileName.empty()) {
        std::string lower = toLower(fileName);
        size_t dot = lower.rfind('.');
        std::string ext = dot == std::string::npos ? lower : lower.substr(dot + 1);
        static const std::map<std::string, InputKind> kinds = {
            {"md", InputKind::Markdown}, {"markdown", InputKind::Markdown}, {"mdown", InputKind::Markdown}, {"mkd", InputKind::Markdown},
            {"txt", InputKind::Text}, {"text", InputKind::Text},
            {"html", InputKind::Html}, {"htm", InputKind::Html}, {"xhtml", InputKind::Html}, {"shtml", InputKind::Html}};
        auto it = kinds.find(ext);
        if (it != kinds.end()) return it->second;
    }
    std::string t = trim(text);
    if (t.empty()) return InputKind::Text;

    // One token without spaces that parses as a URL.
    if (!hasSpace(t) && t.size() < 4096 && t[0] != '<' && t[0] != '{') {
        auto n = normalizeUrl(t);
        static const std::regex httpRe(R"(^https?://)", std::regex::icase);
        static const std::regex hostLike(R"(^[a-z0-9-]+(\.[a-z0-9-]+)+(:\d+)?(/|$|\?|#))", std::regex::icase);
        if (n && (std::regex_search(t, httpRe) || std::regex_search(t, hostLike)))
            return isWikipediaUrl(*n) ? InputKind::Wikipedia : InputKind::Url;
    }

    // A bookmarklet payload as JSON.
    static const std::regex v1(R"("v"\s*:\s*1)");
    static const std::regex htmlKey(R"("html"\s*:)");
    if (t[0] == '{' && std::regex_search(t, v1) && std::regex_search(t, htmlKey)) return InputKind::Bookmarklet;

    // HTML: a document start, or several common tags.
    static const std::regex docStart(R"(^(<\?xml[^>]*>\s*)?(<!doctype html|<html[\s>]))", std::regex::icase);
    if (std::regex_search(t, docStart)) return InputKind::Html;
    static const std::regex tagRe(R"(<(p|div|article|section|body|head|h[1-6]|span|a|br|ul|ol|li|table|img|em|strong|blockquote|pre)\b[^>]*>)", std::regex::icase);
    auto count = std::distance(std::sregex_iterator(t.begin(), t.end(), tagRe), std::sregex_iterator());
    if (count >= 2 || (count == 1 && t[0] == '<')) return InputKind::Html;

    // Markdown: count typical signals.
    int score = 0;
    static const std::regex mdHeading(R"(^#{1,6}\s+\S)");
    static const std::regex mdFence(R"(^(```|~~~))");
    static const std::regex mdLink(R"(\[[^\]\n]+\]\([^)\s]+\))");
    static const std::regex mdBullet(R"(^\s*[-*+]\s+\S)");
    static const std::regex mdNumber(R"(^\s*\d+\.\s+\S)");
    static const std::regex mdStrong(R"((\*\*|__)[^*_\n]+\1)");
    static const std::regex mdQuote(R"(^>\s+\S)");
    static const std::regex mdRule(R"(^(-{3,}|\*{3,}|_{3,})\s*$)");
    static const std::regex mdCode(R"(`[^`\n]+`)");
    static const std::regex mdTableHead(R"(^\|.+\|\s*$)");
    static const std::regex mdTableRule(R"(^\|?\s*:?-{3,})");
    if (anyLine(t, mdHeading)) score += 2;
    if (anyLine(t, mdFence)) score += 2;
    if (std::regex_search(t, mdLink)) score += 2;
    if (anyLine(t, mdBullet)) score += 1;
    if (anyLine(t, mdNumber)) score += 1;
    if (std::regex_search(t, mdStrong)) score += 1;
    if (anyLine(t, mdQuote)) score += 1;
    if (anyLine(t, mdRule)) score += 1;
    if (std::regex_search(t, mdCode)) score += 1;
    {
        std::stringstream ss(t);
        std::string prev, line;
        bool first = true;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!first && std::regex_search(prev, mdTableHead) && std::regex_search(line, mdTableRule)) {
                score += 2;
                break;
            }
            prev = line;
            first = false;
        }
    }
    return score >= 2 ? InputKind::Markdown : InputKind::Text;
}

} // namespace ks::extract
