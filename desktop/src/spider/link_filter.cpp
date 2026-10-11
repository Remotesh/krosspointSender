// K-rosspointSender desktop: spider module, link rules and slugs.
// Port of src/spider/linkFilter.ts and src/spider/slug.ts.
#include "spider/internal.h"
#include "extract/internal.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <regex>
#include <set>

namespace ks::spider {

namespace ex = ks::extract::detail;

namespace {

// Namespace names (lower case) that are not articles. "X talk" is also rejected.
const std::set<std::string> kNonArticleNs = {
    "help", "file", "image", "media", "template", "category", "talk", "special", "portal", "user",
    "wikipedia", "wp", "project", "draft", "module", "mediawiki", "timedtext", "book", "gadget",
    "gadget definition", "topic", "education program",
};

// ------------------------------------------------------------- UTF-8

std::u32string decodeU8(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (len == 0 || i + len > s.size()) {
            ++i;   // Skip a bad byte.
            continue;
        }
        char32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        out += cp;
        i += len;
    }
    return out;
}

std::string encodeU8(char32_t c) {
    std::string o;
    if (c < 0x80) o += static_cast<char>(c);
    else if (c < 0x800) {
        o += static_cast<char>(0xC0 | (c >> 6));
        o += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        o += static_cast<char>(0xE0 | (c >> 12));
        o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        o += static_cast<char>(0xF0 | (c >> 18));
        o += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        o += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (c & 0x3F));
    }
    return o;
}

// Upper case for ASCII, Latin-1 and the pairs of Latin Extended-A.
char32_t upperOf(char32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    bool evenPairs = (c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177);
    bool oddPairs = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
    if (evenPairs && (c & 1) && c != 0x131) return c - 1;
    if (oddPairs && !(c & 1)) return c - 1;
    return c;
}

// ASCII fold of U+00C0..U+017F, like JS toLowerCase + the special folds +
// NFD without marks. "" means "not a letter" (it becomes "_").
const char* const kFoldLatin1[64] = {
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",
};
// U+0100..U+017F. Letters with no canonical decomposition (ij, kra, l-dot,
// n-apostrophe, long s) stay "" like in the web version.
constexpr char kFoldExtA[] =
    "aaaaaacccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "__" "jj" "kk_"
    "ll" "ll" "ll" "__" "ll" "nnnnnn" "_" "nn" "oooooo" "@@" "rrrrrr" "ssssssss" "tttttt"
    "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "_";
static_assert(sizeof(kFoldExtA) == 129, "one entry per code point of U+0100..U+017F");

std::string foldChar(char32_t c) {
    if (c < 0x80) return std::string(1, static_cast<char>(std::tolower(static_cast<int>(c))));
    if (c >= 0xC0 && c <= 0xFF) return kFoldLatin1[c - 0xC0];
    if (c >= 0x100 && c <= 0x17F) {
        char f = kFoldExtA[c - 0x100];
        if (f == '_') return "";
        if (f == '@') return "oe";
        return std::string(1, f);
    }
    if (c >= 0x300 && c <= 0x36F) return "\x01";   // A combining mark: drop it.
    return "";
}

std::mutex gAliasMu;
std::map<std::string, std::string> gAliases;

} // namespace

// ------------------------------------------------------------- public helpers

std::string canonicalTitle(const std::string& raw) {
    std::string t;
    bool space = false;
    for (char c : raw) {
        if (c == '_' || std::isspace(static_cast<unsigned char>(c))) {
            space = true;
            continue;
        }
        if (space && !t.empty()) t += ' ';
        space = false;
        t += c;
    }
    if (t.empty()) return "";
    std::u32string u = decodeU8(t);
    if (u.empty()) return "";
    std::string first = encodeU8(upperOf(u[0]));
    size_t firstLen = encodeU8(u[0]).size();
    return first + t.substr(firstLen);
}

std::string wikipediaArticleUrl(const std::string& lang, const std::string& title) {
    std::string t = title;
    for (char& c : t)
        if (c == ' ') c = '_';
    std::string enc = ex::encodeUriComponent(t);
    std::string out;
    for (size_t i = 0; i < enc.size(); ++i) {
        if (enc.compare(i, 3, "%2F") == 0) { out += '/'; i += 2; }
        else if (enc.compare(i, 3, "%3A") == 0) { out += ':'; i += 2; }
        else out += enc[i];
    }
    return "https://" + lang + ".wikipedia.org/wiki/" + out;
}

std::string packSlug(const std::string& title) {
    std::string s;
    bool gap = false;
    for (char32_t c : decodeU8(title)) {
        std::string f = foldChar(c);
        if (f == "\x01") continue;
        bool alnum = f.size() > 0 && std::all_of(f.begin(), f.end(), [](char x) { return (x >= 'a' && x <= 'z') || (x >= '0' && x <= '9'); });
        if (!alnum) {
            gap = true;
            continue;
        }
        if (gap && !s.empty()) s += '_';
        gap = false;
        s += f;
    }
    if (s.size() > 48) s = s.substr(0, 48);
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? "page" : s;
}

std::string webKey(const std::string& href, const std::string& base) {
    std::string h = trim(href);
    static const std::regex badScheme(R"(^(javascript|vbscript|data|mailto|tel|file|blob|about):)", std::regex::icase);
    if (h.empty() || std::regex_search(h, badScheme)) return "";
    std::string abs = base.empty() ? h : ex::resolveUrl(h, base);
    auto u = ex::parseAbsUrl(abs);
    if (!u || (u->scheme != "http" && u->scheme != "https") || u->host.empty()) return "";
    u->hasFragment = false;
    u->fragment.clear();
    if (u->hasQuery) {
        static const std::set<std::string> tracking = {"fbclid", "gclid", "dclid", "msclkid", "mc_cid", "mc_eid", "ref_src", "igshid"};
        std::string kept;
        size_t start = 0;
        while (start <= u->query.size()) {
            size_t amp = u->query.find('&', start);
            std::string piece = u->query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
            std::string key = toLower(ex::percentDecode(piece.substr(0, piece.find('=')), true));
            if (!piece.empty() && key.rfind("utm_", 0) != 0 && !tracking.count(key)) kept += (kept.empty() ? "" : "&") + piece;
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
        u->query = kept;
        u->hasQuery = !kept.empty();
    }
    if (u->path.size() > 1) {
        while (u->path.size() > 1 && u->path.back() == '/') u->path.pop_back();
        if (u->path.empty()) u->path = "/";
    }
    return u->toString();
}

void rememberAlias(const std::string& lang, const std::string& from, const std::string& to) {
    std::string f = canonicalTitle(from), t = canonicalTitle(to);
    if (f.empty() || t.empty() || f == t) return;
    std::lock_guard<std::mutex> lock(gAliasMu);
    gAliases[lang + "|" + f] = t;
}

void clearAliases() {
    std::lock_guard<std::mutex> lock(gAliasMu);
    gAliases.clear();
}

// ------------------------------------------------------------- internal helpers

namespace detail {

const char* const kNonBodySelector =
    ".navbox, .vertical-navbox, .sidebar, .infobox, .reflist, .mw-references-wrap, sup.reference, .reference, "
    ".hatnote, .catlinks, #toc, .toc, .mw-editsection, .mw-cite-backlink, .metadata, .ambox, .navbox-styles, "
    ".mw-authority-control, .portal, .sistersitebox, ol.references";

std::string resolveAlias(const std::string& lang, const std::string& title) {
    std::lock_guard<std::mutex> lock(gAliasMu);
    auto it = gAliases.find(lang + "|" + title);
    return it == gAliases.end() ? title : it->second;
}

std::string safeDecode(const std::string& s) {
    std::string d = ex::percentDecode(s);
    // decodeURIComponent throws on bad UTF-8. Keep the raw text then.
    std::u32string u = decodeU8(d);
    std::string back;
    for (char32_t c : u) back += encodeU8(c);
    return back == d ? d : s;
}

bool hasNonArticleNamespace(const std::string& title) {
    size_t i = title.find(':');
    if (i == std::string::npos || i == 0) return false;
    std::string prefix = title.substr(0, i);
    for (char& c : prefix)
        if (c == '_') c = ' ';
    prefix = toLower(trim(prefix));
    return kNonArticleNs.count(prefix) > 0 || (prefix.size() >= 5 && prefix.compare(prefix.size() - 5, 5, " talk") == 0);
}

WikiHref classifyWikiHref(const std::string& href, const std::string& pageLang) {
    WikiHref r;
    std::string h = trim(href);
    if (h.empty()) return r;
    if (h[0] == '#') {
        r.type = WikiHref::Fragment;
        r.fragment = h.substr(1);
        return r;
    }
    static const std::regex script(R"(^(javascript|vbscript|data):)", std::regex::icase);
    if (std::regex_search(h, script)) return r;
    std::string abs = h.rfind("./", 0) == 0 ? "/wiki/" + h.substr(2) : h;
    if (abs.rfind("//", 0) == 0) abs = "https:" + abs;
    abs = ex::resolveUrl(abs, "https://" + pageLang + ".wikipedia.org/wiki/");
    auto u = ex::parseAbsUrl(abs);
    if (!u) return r;
    if (u->scheme != "http" && u->scheme != "https") {
        r.type = WikiHref::Other;
        return r;
    }
    static const std::regex hostRe(R"(^(?:([a-z-]+)\.)?(?:m\.)?wikipedia\.org$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(u->host, m, hostRe)) {
        r.type = WikiHref::Other;
        return r;
    }
    std::string lang = m[1].matched && toLower(m[1].str()) != "m" ? toLower(m[1].str()) : "en";
    std::string fragment = safeDecode(u->fragment);
    std::map<std::string, std::string> q;
    size_t start = 0;
    while (u->hasQuery && start <= u->query.size()) {
        size_t amp = u->query.find('&', start);
        std::string piece = u->query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        size_t eq = piece.find('=');
        std::string key = ex::percentDecode(piece.substr(0, eq), true);
        if (!q.count(key)) q[key] = eq == std::string::npos ? "" : ex::percentDecode(piece.substr(eq + 1), true);
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    for (const char* k : {"action", "redlink", "oldid", "diff", "veaction"})
        if (q.count(k)) return r;
    std::string raw;
    if (u->path.rfind("/wiki/", 0) == 0 && u->path.size() > 6) raw = safeDecode(u->path.substr(6));
    else if (u->path == "/w/index.php" && q.count("title")) raw = q["title"];
    if (raw.empty()) return r;
    std::string title = canonicalTitle(raw);
    if (title.empty() || hasNonArticleNamespace(title)) return r;
    r.type = WikiHref::Article;
    r.lang = lang;
    r.title = title;
    r.fragment = fragment;
    return r;
}

bool isNonPageUrl(const std::string& url) {
    auto u = ex::parseAbsUrl(url);
    if (!u) return true;
    static const std::regex ext(
        R"(\.(pdf|jpe?g|png|gif|webp|svg|bmp|ico|zip|gz|tgz|rar|7z|mp3|mp4|m4a|ogg|wav|webm|mov|avi|exe|dmg|apk|docx?|xlsx?|pptx?|epub|mobi|css|js|json|xml|rss|atom)$)",
        std::regex::icase);
    return std::regex_search(u->path, ext);
}

std::string siteHost(const std::string& url) {
    auto u = ex::parseAbsUrl(url);
    if (!u) return "";
    std::string h = toLower(u->host);
    return h.rfind("www.", 0) == 0 ? h.substr(4) : h;
}

} // namespace detail

} // namespace ks::spider
