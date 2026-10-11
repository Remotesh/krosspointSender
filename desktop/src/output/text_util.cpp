// Small text helpers for the output module (port of converters/util.ts).
#include "output/internal.h"

#include <cctype>
#include <cstdio>

namespace ks::output::detail {

std::u32string toU32(const std::string& s) {
    std::u32string out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            ++i;  // Invalid lead byte: skip it.
            continue;
        }
        if (i + len > n) {
            ++i;
            continue;
        }
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        // Reject overlong forms, surrogates and values above U+10FFFF.
        if (ok && ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
                   (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF))
            ok = false;
        if (!ok) {
            ++i;
            continue;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string toU8(char32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

std::string toU8(const std::u32string& s) {
    std::string out;
    out.reserve(s.size());
    for (char32_t c : s) out += toU8(c);
    return out;
}

bool isJsSpace(char32_t c) {
    switch (c) {
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
        case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F:
        case 0x205F: case 0x3000: case 0xFEFF:
            return true;
        default:
            return c >= 0x2000 && c <= 0x200A;
    }
}

size_t cpLength(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

static bool xmlCharOk(char32_t c) {
    return c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) ||
           (c >= 0x10000 && c <= 0x10FFFF);
}

std::string cleanXmlText(const std::string& s) {
    // Fast path: printable ASCII, tabs and newlines.
    bool simple = true;
    for (unsigned char c : s) {
        if (c >= 0x80 || (c < 0x20 && c != 0x9 && c != 0xA && c != 0xD)) {
            simple = false;
            break;
        }
    }
    if (simple) return s;
    std::u32string u = toU32(s);
    std::u32string out;
    out.reserve(u.size());
    for (char32_t c : u)
        if (xmlCharOk(c)) out.push_back(c);
    return toU8(out);
}

std::string escapeXml(const std::string& s) { return ks::xmlEscape(cleanXmlText(s)); }

std::string collapseWs(const std::string& s) {
    std::u32string u = toU32(cleanXmlText(s));
    std::u32string out;
    out.reserve(u.size());
    bool space = false;
    for (char32_t c : u) {
        if (isJsSpace(c)) {
            space = true;
            continue;
        }
        if (space && !out.empty()) out.push_back(U' ');
        space = false;
        out.push_back(c);
    }
    return toU8(out);
}

std::string jsTrim(const std::string& s) {
    std::u32string u = toU32(s);
    size_t a = 0, b = u.size();
    while (a < b && isJsSpace(u[a])) ++a;
    while (b > a && isJsSpace(u[b - 1])) --b;
    return toU8(u.substr(a, b - a));
}

std::string isoDay(const std::string& iso) {
    // The web version also accepts any Date.parse() string. Here we accept
    // a leading YYYY-MM-DD only. All producers in this app write ISO dates.
    if (iso.size() < 10) return "";
    for (size_t i = 0; i < 10; ++i) {
        char c = iso[i];
        if (i == 4 || i == 7) {
            if (c != '-') return "";
        } else if (!std::isdigit(static_cast<unsigned char>(c))) {
            return "";
        }
    }
    return iso.substr(0, 10);
}

std::string langOf(const Article& a) {
    // Pattern: ^[A-Za-z]{2,3}(-[A-Za-z0-9]{1,8})*$
    std::string l = trim(a.language);
    auto alpha = [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; };
    auto alnum = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    size_t i = 0;
    while (i < l.size() && alpha(l[i])) ++i;
    if (i < 2 || i > 3) return "en";
    while (i < l.size()) {
        if (l[i] != '-') return "en";
        ++i;
        size_t start = i;
        while (i < l.size() && alnum(l[i])) ++i;
        size_t len = i - start;
        if (len < 1 || len > 8) return "en";
    }
    return l;
}

std::string pad(int n, int width) {
    std::string s = std::to_string(n);
    while (static_cast<int>(s.size()) < width) s.insert(s.begin(), '0');
    return s;
}

static bool isUuid(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::string bookIdentifier(const std::string& id) {
    return "urn:uuid:" + (isUuid(id) ? toLower(id) : newUuid());
}

std::string sourceNoteText(const Article& a) {
    std::string src = trim(a.sourceUrl);
    if (src.empty()) src = toString(a.inputKind) + " input";
    std::string day = isoDay(a.capturedAt);
    if (day.empty()) day = isoDay(nowIso());
    return "Source: " + src + ", captured " + day + ", generated by " + kAppName + ".";
}

int countTextWords(const std::string& text) {
    std::u32string u = toU32(text);
    int n = 0;
    bool in = false;
    for (char32_t c : u) {
        if (isJsSpace(c)) {
            in = false;
        } else if (!in) {
            in = true;
            ++n;
        }
    }
    return n;
}

std::string repeatStr(const std::string& s, size_t n) {
    std::string out;
    out.reserve(s.size() * n);
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find('\n', start);
        if (p == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, p - start));
        start = p + 1;
    }
    return out;
}

// Formats a number with "," thousands separators (en-US).
static std::string groupThousands(int v) {
    std::string s = std::to_string(v < 0 ? -v : v);
    std::string out;
    int k = 0;
    for (size_t i = s.size(); i > 0; --i) {
        out.insert(out.begin(), s[i - 1]);
        if (++k % 3 == 0 && i > 1) out.insert(out.begin(), ',');
    }
    return v < 0 ? "-" + out : out;
}

std::vector<Line> coverLines(const Article& a) {
    std::vector<Line> lines;
    auto add = [&](const char* label, const std::string& value) {
        std::string v = collapseWs(value);
        if (!v.empty()) lines.push_back({label, v});
    };
    add("Author", a.author);
    add("Site", a.siteName);
    add("Published", isoDay(a.publishedAt));
    add("Source", a.sourceUrl);
    add("Captured", isoDay(a.capturedAt));
    if (a.wordCount > 0) add("Words", groupThousands(a.wordCount));
    return lines;
}

} // namespace ks::output::detail
