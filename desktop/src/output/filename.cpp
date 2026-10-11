// Filename rules (port of converters/filename.ts). The output must be safe
// on FAT32/exFAT SD cards, Windows, macOS and Linux.
#include "output/internal.h"

#include <cctype>
#include <map>

namespace ks::output {

using detail::isJsSpace;
using detail::toU32;
using detail::toU8;

namespace {

// Reserved characters and control characters.
bool isReserved(char32_t c) {
    switch (c) {
        case '<': case '>': case ':': case '"': case '/': case '\\': case '|': case '?': case '*':
            return true;
        default:
            return c <= 0x1F || (c >= 0x7F && c <= 0x9F);
    }
}

// Separator characters of the makeFilename() cleanup: - _ en dash, em dash , ; middle dot |
bool isSep(char32_t c) {
    return c == U'-' || c == U'_' || c == 0x2013 || c == 0x2014 || c == U',' || c == U';' || c == 0xB7 || c == U'|';
}

std::u32string trimEdges(const std::u32string& s, const std::u32string& extra) {
    auto in = [&](char32_t c) { return c == U'.' || c == U' ' || extra.find(c) != std::u32string::npos; };
    size_t a = 0, b = s.size();
    while (a < b && in(s[a])) ++a;
    while (b > a && in(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Simple Unicode lower-case: ASCII, Latin-1, Latin Extended-A, Greek, Cyrillic.
char32_t lowerCp(char32_t c) {
    if (c >= U'A' && c <= U'Z') return c + 32;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x100 && c <= 0x137 && (c % 2) == 0) return c + 1;
    if (c >= 0x139 && c <= 0x148 && (c % 2) == 1) return c + 1;
    if (c >= 0x14A && c <= 0x177 && (c % 2) == 0) return c + 1;
    if (c == 0x178) return 0xFF;
    if (c == 0x179 || c == 0x17B || c == 0x17D) return c + 1;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

bool isDeviceName(const std::u32string& s) {
    std::string l;
    for (char32_t c : s) {
        if (c > 0x7F) return false;
        l += static_cast<char>(lowerCp(c));
    }
    if (l == "con" || l == "prn" || l == "aux" || l == "nul") return true;
    if (l.size() == 4 && (l.compare(0, 3, "com") == 0 || l.compare(0, 3, "lpt") == 0) && l[3] >= '0' && l[3] <= '9')
        return true;
    return false;
}

} // namespace

std::string sanitizeFilename(const std::string& name, const FilenameRules& r) {
    const size_t maxLength = r.maxLength > 0 ? static_cast<size_t>(r.maxLength) : 80;
    // The replacement must itself be safe.
    std::u32string repl;
    for (char32_t c : toU32(r.spaceReplacement))
        if (!isReserved(c) && !isJsSpace(c)) repl.push_back(c);

    // Note: the web version applies NFC normalization here. We do not have
    // ICU, so the text stays in the form we got (mostly NFC already).
    std::u32string in = toU32(name);
    std::u32string s;
    s.reserve(in.size());
    bool space = false;
    for (char32_t c : in) {
        if (isReserved(c)) c = U' ';
        if (isJsSpace(c)) {
            space = true;
            continue;
        }
        if (space) s.push_back(U' ');
        space = false;
        s.push_back(c);
    }
    if (space) s.push_back(U' ');
    s = trimEdges(s, U"");
    if (r.lowercase)
        for (char32_t& c : s) c = lowerCp(c);
    if (!repl.empty()) {
        std::u32string t;
        for (char32_t c : s) {
            if (c == U' ') t += repl;
            else t.push_back(c);
        }
        // Collapse runs of the replacement, e.g. "a - b" with "-" gives "a---b".
        std::u32string u;
        size_t i = 0;
        while (i < t.size()) {
            if (t.compare(i, repl.size(), repl) == 0) {
                size_t j = i;
                while (j < t.size() && t.compare(j, repl.size(), repl) == 0) j += repl.size();
                u += repl;
                i = j;
            } else {
                u.push_back(t[i++]);
            }
        }
        s = u;
    }
    if (s.size() > maxLength) s.resize(maxLength);
    s = trimEdges(s, repl);
    if (isDeviceName(s)) s.insert(s.begin(), U'_');
    std::string out = toU8(s);
    return out.empty() ? "untitled" : out;
}

std::string makeFilename(const Article& a, OutputFormat f, const FilenameRules& r) {
    const std::string date = !detail::isoDay(a.publishedAt).empty() ? detail::isoDay(a.publishedAt)
                                                                    : detail::isoDay(a.capturedAt);
    const std::map<std::string, std::string> tokens = {
        {"title", a.title},
        {"author", a.author},
        {"site", a.siteName},
        {"date", date},
        {"published", detail::isoDay(a.publishedAt)},
        {"captured", detail::isoDay(a.capturedAt)},
        {"year", date.substr(0, 4)},
        {"lang", a.language},
        {"id", a.id.substr(0, 8)},
    };
    std::string templ = detail::jsTrim(r.templ);
    if (templ.empty()) templ = "{title}";

    // Replace {word} tokens. Unknown tokens become "".
    std::string s;
    for (size_t i = 0; i < templ.size();) {
        if (templ[i] == '{') {
            size_t j = i + 1;
            while (j < templ.size() && (std::isalnum(static_cast<unsigned char>(templ[j])) || templ[j] == '_')) ++j;
            if (j > i + 1 && j < templ.size() && templ[j] == '}') {
                auto it = tokens.find(toLower(templ.substr(i + 1, j - i - 1)));
                if (it != tokens.end()) s += it->second;
                i = j + 1;
                continue;
            }
        }
        s += templ[i++];
    }

    // Clean up separators that an empty token left behind.
    std::u32string u = toU32(s);
    // 1. "()" and "[]" with only whitespace inside become one space.
    {
        std::u32string t;
        for (size_t i = 0; i < u.size(); ++i) {
            if (u[i] == U'(' || u[i] == U'[') {
                char32_t close = u[i] == U'(' ? U')' : U']';
                size_t j = i + 1;
                while (j < u.size() && isJsSpace(u[j])) ++j;
                if (j < u.size() && u[j] == close) {
                    t.push_back(U' ');
                    i = j;
                    continue;
                }
            }
            t.push_back(u[i]);
        }
        u = t;
    }
    // 2. A run of whitespace and separators with two or more separators
    //    becomes " - " (when the run holds a space) or its first separator.
    {
        std::u32string t;
        size_t i = 0;
        while (i < u.size()) {
            if (isJsSpace(u[i]) || isSep(u[i])) {
                size_t j = i;
                int seps = 0;
                bool hasSpace = false;
                char32_t first = 0;
                while (j < u.size() && (isJsSpace(u[j]) || isSep(u[j]))) {
                    if (isSep(u[j])) {
                        if (!seps) first = u[j];
                        ++seps;
                    }
                    if (u[j] == U' ') hasSpace = true;
                    ++j;
                }
                if (seps >= 2) {
                    if (hasSpace) t += U" - ";
                    else t.push_back(first);
                } else {
                    t.append(u, i, j - i);
                }
                i = j;
            } else {
                t.push_back(u[i++]);
            }
        }
        u = t;
    }
    // 3. Leading separators.
    {
        size_t i = 0;
        while (i < u.size() && isJsSpace(u[i])) ++i;
        if (i < u.size() && isSep(u[i])) {
            while (i < u.size() && isSep(u[i])) ++i;
            while (i < u.size() && isJsSpace(u[i])) ++i;
            u.erase(0, i);
        }
    }
    // 4. Trailing separators.
    {
        size_t j = u.size();
        while (j > 0 && isJsSpace(u[j - 1])) --j;
        if (j > 0 && isSep(u[j - 1])) {
            while (j > 0 && isSep(u[j - 1])) --j;
            while (j > 0 && isJsSpace(u[j - 1])) --j;
            u.erase(j);
        }
    }
    s = toU8(u);
    if (detail::jsTrim(s).empty()) s = a.title.empty() ? "untitled" : a.title;
    return sanitizeFilename(s, r) + "." + extensionFor(f);
}

} // namespace ks::output
