// K-rosspointSender desktop: extract module, page metadata.
// Reads <meta> tags, <time> elements and JSON-LD. Port of src/extractors/meta.ts.
#include "extract/internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <list>
#include <regex>

namespace ks::extract::detail {

using json = nlohmann::json;

// ============================================================= dates

static long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}

static void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<long long>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

static unsigned daysInMonth(long long y, unsigned m) {
    static const unsigned dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

// Format a UTC moment as "2025-03-14T09:30:00.000Z". offsetMin is the zone of the input.
static std::string formatIso(long long y, unsigned mo, unsigned d, int h, int mi, int s, int ms, int offsetMin) {
    if (mo < 1 || mo > 12 || d < 1 || d > daysInMonth(y, mo) || h > 24 || mi > 59 || s > 60) return "";
    long long secs = daysFromCivil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + s - offsetMin * 60LL;
    long long days = secs >= 0 ? secs / 86400 : (secs - 86399) / 86400;
    long long rem = secs - days * 86400;
    long long yy;
    unsigned mm, dd;
    civilFromDays(days, yy, mm, dd);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02u-%02uT%02d:%02d:%02d.%03dZ", yy, mm, dd, static_cast<int>(rem / 3600),
                  static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60), ms);
    return buf;
}

static int monthFromName(const std::string& w) {
    static const char* names[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (w.size() < 3) return 0;
    for (int i = 0; i < 12; ++i)
        if (w.compare(0, 3, names[i]) == 0) return i + 1;
    return 0;
}

std::string toIsoDate(const std::string& s) {
    std::string v = normText(s);
    if (v.empty()) return "";
    std::smatch m;
    // ISO 8601.
    static const std::regex iso(R"(^([+-]?\d{4,6})-(\d{2})-(\d{2})(?:[Tt ](\d{2}):(\d{2})(?::(\d{2})(?:[.,](\d+))?)?)?\s*(Z|z|UTC|GMT|[+-]\d{2}(?::?\d{2})?)?$)");
    if (std::regex_match(v, m, iso)) {
        int offset = 0;
        std::string z = m[8].str();
        if (!z.empty() && (z[0] == '+' || z[0] == '-')) {
            std::string digits;
            for (char c : z)
                if (std::isdigit(static_cast<unsigned char>(c))) digits += c;
            int hh = std::stoi(digits.substr(0, 2));
            int mm = digits.size() >= 4 ? std::stoi(digits.substr(2, 2)) : 0;
            offset = (hh * 60 + mm) * (z[0] == '-' ? -1 : 1);
        }
        int ms = 0;
        if (m[7].matched) {
            std::string frac = (m[7].str() + "000").substr(0, 3);
            ms = std::stoi(frac);
        }
        return formatIso(std::stoll(m[1].str()), static_cast<unsigned>(std::stoi(m[2].str())), static_cast<unsigned>(std::stoi(m[3].str())),
                         m[4].matched ? std::stoi(m[4].str()) : 0, m[5].matched ? std::stoi(m[5].str()) : 0,
                         m[6].matched ? std::stoi(m[6].str()) : 0, ms, offset);
    }
    static const std::regex yearMonth(R"(^(\d{4})(?:-(\d{2}))?$)");
    if (std::regex_match(v, m, yearMonth))
        return formatIso(std::stoll(m[1].str()), m[2].matched ? static_cast<unsigned>(std::stoi(m[2].str())) : 1, 1, 0, 0, 0, 0, 0);
    static const std::regex slash(R"(^(\d{1,2})/(\d{1,2})/(\d{4})$)");
    if (std::regex_match(v, m, slash))
        return formatIso(std::stoll(m[3].str()), static_cast<unsigned>(std::stoi(m[1].str())), static_cast<unsigned>(std::stoi(m[2].str())), 0, 0, 0, 0, 0);

    // Text dates: "Fri, 14 Mar 2025 09:30:00 GMT", "March 14, 2025", "14 March 2025 10:00 +0100".
    std::string low = toLower(v);
    for (char& c : low)
        if (c == ',') c = ' ';
    auto words = splitWords(low);
    long long year = -1;
    int month = 0, day = 0, h = 0, mi = 0, sec = 0, offset = 0;
    bool pm = false, am = false;
    static const std::regex timeRe(R"(^(\d{1,2}):(\d{2})(?::(\d{2}))?(am|pm)?$)");
    static const std::regex zoneRe(R"(^(?:gmt|utc)?([+-])(\d{2}):?(\d{2})$)");
    static const std::regex dayRe(R"(^(\d{1,2})(st|nd|rd|th)?\.?$)");
    for (const auto& w : words) {
        std::smatch t;
        if (std::regex_match(w, t, timeRe)) {
            h = std::stoi(t[1].str());
            mi = std::stoi(t[2].str());
            sec = t[3].matched ? std::stoi(t[3].str()) : 0;
            if (t[4].matched) (t[4] == "pm" ? pm : am) = true;
        } else if (std::regex_match(w, t, zoneRe)) {
            offset = (std::stoi(t[2].str()) * 60 + std::stoi(t[3].str())) * (t[1] == "-" ? -1 : 1);
        } else if (w == "gmt" || w == "utc" || w == "ut" || w == "z") {
            offset = 0;
        } else if (w == "est" || w == "edt" || w == "cst" || w == "cdt" || w == "mst" || w == "mdt" || w == "pst" || w == "pdt") {
            static const std::map<std::string, int> zones = {{"est", -300}, {"edt", -240}, {"cst", -360}, {"cdt", -300},
                                                             {"mst", -420}, {"mdt", -360}, {"pst", -480}, {"pdt", -420}};
            offset = zones.at(w);
        } else if (w == "am") {
            am = true;
        } else if (w == "pm") {
            pm = true;
        } else if (w.size() == 4 && std::isdigit(static_cast<unsigned char>(w[0])) && std::regex_match(w, std::regex(R"(\d{4})"))) {
            year = std::stoll(w);
        } else if (std::regex_match(w, t, dayRe)) {
            if (!day) day = std::stoi(t[1].str());
        } else if (int mo = monthFromName(w)) {
            if (!month) month = mo;
        }
    }
    if (pm && h < 12) h += 12;
    if (am && h == 12) h = 0;
    if (year < 0 || !month) return "";
    if (!day) day = 1;
    return formatIso(year, static_cast<unsigned>(month), static_cast<unsigned>(day), h, mi, sec, 0, offset);
}

// ============================================================= JSON-LD

static std::vector<std::string> typesOf(const json& o) {
    std::vector<std::string> out;
    auto it = o.find("@type");
    if (it == o.end()) return out;
    if (it->is_string()) out.push_back(it->get<std::string>());
    else if (it->is_array())
        for (const auto& t : *it)
            if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}

static std::string nameOf(const json* v) {
    if (!v || v->is_null()) return "";
    if (v->is_string()) return normText(v->get<std::string>());
    if (v->is_array()) {
        std::vector<std::string> names;
        for (const auto& x : *v) {
            std::string n = nameOf(&x);
            if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
        }
        std::string out;
        for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
        return out;
    }
    if (v->is_object()) {
        auto it = v->find("name");
        if (it != v->end() && it->is_string()) return normText(it->get<std::string>());
    }
    return "";
}

static const json* field(const json& o, const char* key) {
    auto it = o.find(key);
    return it == o.end() ? nullptr : &*it;
}

static std::string strField(const json& o, const char* key) {
    const json* f = field(o, key);
    return f && f->is_string() ? f->get<std::string>() : std::string();
}

static bool hasStrField(const json& o, const char* key) {
    const json* f = field(o, key);
    return f && f->is_string();
}

JsonLd readJsonLd(Dom& dom) {
    std::list<json> docs;
    std::vector<const json*> found;
    std::function<void(const json&, int)> visit = [&](const json& v, int depth) {
        if (depth > 6) return;
        if (v.is_array()) {
            for (const auto& x : v) visit(x, depth + 1);
        } else if (v.is_object()) {
            found.push_back(&v);
            if (const json* g = field(v, "@graph")) visit(*g, depth + 1);
            if (const json* me = field(v, "mainEntity"))
                if (me->is_object()) visit(*me, depth + 1);
        }
    };
    for (Node s : querySelectorAll(dom.root(), "script")) {
        if (!equalsNoCase(trim(attr(s, "type")), "application/ld+json")) continue;
        std::string text = trim(textContent(s));
        if (text.rfind("<!--", 0) == 0) text = text.substr(4);
        if (text.size() >= 3 && text.compare(text.size() - 3, 3, "-->") == 0) text.resize(text.size() - 3);
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded()) continue;   // Bad JSON-LD is common. Skip it.
        docs.push_back(std::move(j));
        visit(docs.back(), 0);
    }
    static const std::regex articleTypes(
        "^(Article|NewsArticle|BlogPosting|Report|ScholarlyArticle|TechArticle|ReportageNewsArticle|AnalysisNewsArticle|"
        "OpinionNewsArticle|LiveBlogPosting|SocialMediaPosting|Review|Recipe|HowTo)$");
    const json* best = nullptr;
    for (const json* o : found) {
        for (const auto& t : typesOf(*o))
            if (std::regex_match(t, articleTypes)) { best = o; break; }
        if (best) break;
    }
    if (!best)
        for (const json* o : found)
            if (hasStrField(*o, "headline")) { best = o; break; }
    if (!best)
        for (const json* o : found) {
            auto ts = typesOf(*o);
            if (std::find(ts.begin(), ts.end(), "WebPage") != ts.end()) { best = o; break; }
        }
    JsonLd out;
    if (!best) return out;
    const json& b = *best;
    out.headline = hasStrField(b, "headline") ? normText(strField(b, "headline")) : hasStrField(b, "name") ? normText(strField(b, "name")) : "";
    out.author = nameOf(field(b, "author"));
    if (out.author.empty()) out.author = nameOf(field(b, "creator"));
    out.datePublished = hasStrField(b, "datePublished") ? strField(b, "datePublished") : strField(b, "dateCreated");
    out.publisher = nameOf(field(b, "publisher"));
    out.description = normText(strField(b, "description"));
    out.inLanguage = strField(b, "inLanguage");
    return out;
}

// ============================================================= meta tags

static std::string metaContent(Dom& dom, std::initializer_list<const char*> keys) {
    auto metas = querySelectorAll(dom.root(), "meta");
    for (const char* k : keys) {
        for (Node m : metas) {
            if (equalsNoCase(attr(m, "property"), k) || equalsNoCase(attr(m, "name"), k) || equalsNoCase(attr(m, "itemprop"), k)) {
                std::string v = normText(attr(m, "content"));
                if (!v.empty()) return v;
                break;   // The first match decides, like querySelector.
            }
        }
    }
    return "";
}

static std::string pageTitle(Dom& dom) {
    Node t = querySelector(dom.root(), "title");
    return t ? normText(textContent(t)) : "";
}

static std::string firstNonEmpty(std::initializer_list<std::string> v) {
    for (const auto& s : v)
        if (!s.empty()) return s;
    return "";
}

PageMeta readMeta(Dom& dom) {
    JsonLd ld = readJsonLd(dom);
    Node root = dom.root();
    std::string metaAuthor = metaContent(dom, {"author", "article:author", "dc.creator", "dcterms.creator", "parsely-author", "sailthru.author", "twitter:creator"});
    Node relEl = querySelector(root, "[rel=\"author\"], [itemprop=\"author\"] [itemprop=\"name\"], [itemprop=\"author\"], .byline .author, .author-name");
    std::string relAuthor = relEl ? normText(textContent(relEl)) : "";
    std::string authorFromMeta = !metaAuthor.empty() && !startsWithNoCase(metaAuthor, "http://") && !startsWithNoCase(metaAuthor, "https://") && metaAuthor[0] != '@'
                                     ? metaAuthor : "";

    Node timeEl = querySelector(root, "time[datetime]");
    std::string published = toIsoDate(ld.datePublished);
    if (published.empty())
        published = toIsoDate(metaContent(dom, {"article:published_time", "og:published_time", "datePublished", "date", "dc.date", "dc.date.issued",
                                                "dcterms.created", "pubdate", "publishdate", "publish-date", "sailthru.date", "parsely-pub-date"}));
    if (published.empty() && timeEl) published = toIsoDate(attr(timeEl, "datetime"));
    if (published.empty()) {
        Node ip = querySelector(root, "[itemprop=\"datePublished\"]");
        if (ip) published = toIsoDate(attr(ip, "content"));
    }

    PageMeta out;
    for (Node m : querySelectorAll(root, "meta[property=\"article:tag\" i]")) {
        std::string t = normText(attr(m, "content"));
        if (t.empty()) continue;
        if (out.tags.size() >= 10) break;
        if (std::find(out.tags.begin(), out.tags.end(), t) == out.tags.end()) out.tags.push_back(t);
    }

    std::string lang = normText(attr(root, "lang"));
    if (lang.empty()) lang = metaContent(dom, {"og:locale", "language", "content-language"});
    if (lang.empty()) lang = ld.inLanguage;
    size_t us = lang.find('_');
    if (us != std::string::npos) lang[us] = '-';

    out.title = firstNonEmpty({ld.headline, metaContent(dom, {"og:title", "twitter:title", "dc.title", "parsely-title"}), pageTitle(dom)});
    std::string rel = relAuthor;
    if (startsWithNoCase(rel, "by ")) rel = trim(rel.substr(3));
    out.author = firstNonEmpty({ld.author, authorFromMeta, !relAuthor.empty() && u8len(relAuthor) < 100 ? rel : std::string()});
    out.publishedAt = published;
    out.siteName = firstNonEmpty({metaContent(dom, {"og:site_name", "application-name", "apple-mobile-web-app-title"}), ld.publisher});
    out.excerpt = firstNonEmpty({metaContent(dom, {"og:description", "description", "twitter:description"}), ld.description});
    out.language = lang;
    return out;
}

} // namespace ks::extract::detail
