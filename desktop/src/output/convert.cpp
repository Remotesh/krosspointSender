// HTML tree to plain text (port of converters/text.ts) and to Markdown
// (a small walker in place of turndown: ATX headings, fenced code, "-"
// bullets, pipe tables).
#include "output/internal.h"

#include <algorithm>
#include <set>

namespace ks::output::detail {

namespace {

// Replaces each run of JS whitespace with one space. No trim.
std::string spaceRuns(const std::string& s) {
    std::u32string u = toU32(s), out;
    bool sp = false;
    for (char32_t c : u) {
        if (isJsSpace(c)) {
            if (!sp) out.push_back(U' ');
            sp = true;
        } else {
            out.push_back(c);
            sp = false;
        }
    }
    return toU8(out);
}

// Collapses spaces in each line and trims. Keeps the line breaks from <br>.
std::string tidy(const std::string& s) {
    std::vector<std::string> lines = splitLines(s);
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::u32string u = toU32(lines[i]), out;
        bool sp = false;
        for (char32_t c : u) {
            bool w = c == U' ' || c == U'\t' || c == U'\f' || c == U'\v' || c == U'\r' || c == 0xA0;
            if (w) {
                if (!sp) out.push_back(U' ');
                sp = true;
            } else {
                out.push_back(c);
                sp = false;
            }
        }
        if (i) joined += "\n";
        joined += jsTrim(toU8(out));
    }
    // \n{3,} -> \n\n
    std::string r;
    int nl = 0;
    for (char c : joined) {
        if (c == '\n') {
            if (++nl <= 2) r += c;
        } else {
            nl = 0;
            r += c;
        }
    }
    return jsTrim(r);
}

std::string joinStr(const std::vector<std::string>& v, const std::string& sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

std::string prefixLines(const std::string& text, const std::string& first, const std::string& rest) {
    std::vector<std::string> lines = splitLines(text);
    std::string restTrim = rest;
    while (!restTrim.empty() && restTrim.back() == ' ') restTrim.pop_back();
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += (i == 0 ? first : (lines[i].empty() ? restTrim : rest)) + lines[i];
    }
    return out;
}

bool startsWithI(const std::string& s, const char* p) {
    std::string l = toLower(s.substr(0, std::char_traits<char>::length(p)));
    return l == p;
}

bool isLinkHref(const std::string& href) {
    return startsWithI(href, "http:") || startsWithI(href, "https:") || startsWithI(href, "mailto:");
}

// ================================================================= plain text

struct TxtOpts {
    bool keepLinks = true;
    bool includeImages = true;
};

const std::set<std::string> kTxtBlock = {
    "p", "div", "section", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
    "pre", "table", "thead", "tbody", "tfoot", "tr", "caption", "dl", "dt", "dd", "hr", "main",
    "header", "footer", "article", "figure",
};

std::vector<std::string> txtBlock(xmlNodePtr e, const TxtOpts& o);

std::string txtInline(xmlNodePtr n, const TxtOpts& o) {
    if (isText(n)) return spaceRuns(textOf(n));
    if (!isElement(n)) return "";
    const std::string tag = nameOf(n);
    if (tag == "br") return "\n";
    if (tag == "img") {
        std::string alt = collapseWs(attrOf(n, "alt"));
        if (!o.includeImages && alt.empty()) return "";
        return alt.empty() ? " [Image] " : " [Image: " + alt + "] ";
    }
    std::string inner;
    for (xmlNodePtr c = n->children; c; c = c->next) inner += txtInline(c, o);
    if (tag == "a") {
        std::string href = trim(attrOf(n, "href"));
        std::string text = collapseWs(inner);
        std::string bare = startsWithI(href, "mailto:") ? href.substr(7) : href;
        if (o.keepLinks && isLinkHref(href) && text != href && text != bare)
            return text.empty() ? href : inner + " (" + href + ")";
        return inner;
    }
    return inner;
}

bool txtIsBlock(xmlNodePtr n) { return isElement(n) && kTxtBlock.count(nameOf(n)) > 0; }

std::vector<std::string> txtBlocks(xmlNodePtr el, const TxtOpts& o) {
    std::vector<std::string> out;
    std::string buf;
    auto flush = [&] {
        std::string t = tidy(buf);
        if (!t.empty()) out.push_back(t);
        buf.clear();
    };
    for (xmlNodePtr c = el->children; c; c = c->next) {
        if (txtIsBlock(c)) {
            flush();
            for (auto& b : txtBlock(c, o)) out.push_back(b);
        } else {
            buf += txtInline(c, o);
        }
    }
    flush();
    return out;
}

std::string txtList(xmlNodePtr list, const TxtOpts& o) {
    const bool ordered = nameOf(list) == "ol";
    int n = 1;
    std::string start = attrOf(list, "start");
    if (!start.empty()) {
        try {
            int v = std::stoi(start);
            if (v > 0) n = v;
        } catch (...) {
        }
    }
    std::vector<std::string> items;
    for (xmlNodePtr li : childElements(list)) {
        std::string content = nameOf(li) == "li" ? joinStr(txtBlocks(li, o), "\n") : joinStr(txtBlock(li, o), "\n");
        if (content.empty()) continue;
        std::string marker = ordered ? std::to_string(n++) + ". " : "- ";
        items.push_back(prefixLines(content, marker, std::string(marker.size(), ' ')));
    }
    return joinStr(items, "\n");
}

std::vector<std::string> txtBlock(xmlNodePtr e, const TxtOpts& o) {
    const std::string tag = nameOf(e);
    if (isHeading(e)) {
        std::string t = collapseWs(joinStr(txtBlocks(e, o), " "));
        if (t.empty()) return {};
        return {t + "\n" + std::string(std::max<size_t>(3, cpLength(t)), tag == "h1" ? '=' : '-')};
    }
    if (tag == "ul" || tag == "ol") {
        std::string t = txtList(e, o);
        if (t.empty()) return {};
        return {t};
    }
    if (tag == "blockquote") {
        std::string inner = joinStr(txtBlocks(e, o), "\n\n");
        if (inner.empty()) return {};
        std::vector<std::string> lines = splitLines(prefixLines(inner, "> ", "> "));
        for (auto& l : lines)
            if (l == "> ") l = ">";
        return {joinStr(lines, "\n")};
    }
    if (tag == "pre") {
        std::string raw;
        std::string src = textOf(e);
        for (size_t i = 0; i < src.size(); ++i) {
            if (src[i] == '\r') {
                raw += '\n';
                if (i + 1 < src.size() && src[i + 1] == '\n') ++i;
            } else {
                raw += src[i];
            }
        }
        size_t a = raw.find_first_not_of('\n');
        if (a == std::string::npos) return {};
        raw = raw.substr(a);
        // Trailing whitespace (JS \s+$).
        std::u32string u = toU32(raw);
        while (!u.empty() && isJsSpace(u.back())) u.pop_back();
        raw = toU8(u);
        if (raw.empty()) return {};
        std::vector<std::string> lines = splitLines(raw);
        for (auto& l : lines) l = "    " + l;
        return {joinStr(lines, "\n")};
    }
    if (tag == "hr") return {"* * *"};
    if (tag == "table") {
        std::vector<std::string> lines;
        auto caps = descendants(e, [](xmlNodePtr x) { return nameOf(x) == "caption"; });
        if (!caps.empty()) {
            std::string c = tidy(txtInline(caps[0], o));
            if (!c.empty()) lines.push_back(c);
        }
        for (xmlNodePtr tr : descendants(e, [](xmlNodePtr x) { return nameOf(x) == "tr"; })) {
            std::vector<std::string> cells;
            for (xmlNodePtr td : childElements(tr)) {
                std::vector<std::string> parts;
                for (xmlNodePtr c = td->children; c; c = c->next) parts.push_back(txtInline(c, o));
                std::string cell = tidy(joinStr(parts, " "));
                std::replace(cell.begin(), cell.end(), '\n', ' ');
                if (!cell.empty()) cells.push_back(cell);
            }
            if (!cells.empty()) lines.push_back(joinStr(cells, " | "));
        }
        if (lines.empty()) return {};
        return {joinStr(lines, "\n")};
    }
    if (tag == "dl") {
        std::vector<std::string> lines;
        for (xmlNodePtr c : childElements(e)) {
            std::string t = joinStr(txtBlocks(c, o), "\n");
            if (t.empty()) continue;
            lines.push_back(nameOf(c) == "dd" ? prefixLines(t, "  ", "  ") : t);
        }
        if (lines.empty()) return {};
        return {joinStr(lines, "\n")};
    }
    return txtBlocks(e, o);
}

// ================================================================= Markdown

const char kBr = '\x01';  // placeholder for a hard line break

std::string mdEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': case '*': case '_': case '`': case '[': case ']':
                out += '\\';
                out += c;
                break;
            default: out += c;
        }
    }
    return out;
}

// Escapes Markdown block markers at the start of a line.
std::string mdEscapeLineStart(const std::string& line) {
    if (line.empty()) return line;
    char c = line[0];
    if (c == '#' || c == '>' || c == '=' || c == '~') return "\\" + line;
    if ((c == '-' || c == '+') && (line.size() == 1 || line[1] == ' ')) return "\\" + line;
    size_t i = 0;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i > 0 && i < line.size() && line[i] == '.' && (i + 1 == line.size() || line[i + 1] == ' '))
        return line.substr(0, i) + "\\" + line.substr(i);
    return line;
}

std::string mdWrap(const std::string& inner, const std::string& mark) {
    std::string t = jsTrim(inner);
    if (t.empty()) return inner;
    // Keep the outer spaces outside the markers.
    size_t lead = inner.find(t);
    std::string pre = inner.substr(0, lead), post = inner.substr(lead + t.size());
    return pre + mark + t + mark + post;
}

std::vector<std::string> mdBlock(xmlNodePtr e);

std::string mdInline(xmlNodePtr n) {
    if (isText(n)) return mdEscape(spaceRuns(textOf(n)));
    if (!isElement(n)) return "";
    const std::string tag = nameOf(n);
    if (tag == "br") return std::string(1, kBr);
    if (tag == "img") {
        std::string alt = collapseWs(attrOf(n, "alt"));
        std::string src = trim(attrOf(n, "src"));
        if (src.empty()) return alt.empty() ? "" : "*[Image: " + mdEscape(alt) + "]*";
        return "![" + mdEscape(alt) + "](" + src + ")";
    }
    if (tag == "code") {
        std::string t = textOf(n);
        if (t.empty()) return "";
        std::string fence = t.find('`') != std::string::npos ? "``" : "`";
        std::string padStr = t.find('`') != std::string::npos ? " " : "";
        return fence + padStr + t + padStr + fence;
    }
    std::string inner;
    for (xmlNodePtr c = n->children; c; c = c->next) inner += mdInline(c);
    if (tag == "strong" || tag == "b") return mdWrap(inner, "**");
    if (tag == "em" || tag == "i") return mdWrap(inner, "*");
    if (tag == "del") return mdWrap(inner, "~~");
    if (tag == "a") {
        std::string href = trim(attrOf(n, "href"));
        if (href.empty()) return inner;
        std::string text = jsTrim(inner);
        return "[" + (text.empty() ? href : text) + "](" + href + ")";
    }
    if (attrOf(n, "class") == "img-alt") return "*[Image: " + jsTrim(inner) + "]*";
    return inner;
}

const std::set<std::string> kMdBlock = {
    "p", "div", "section", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
    "pre", "table", "thead", "tbody", "tfoot", "tr", "caption", "dl", "dt", "dd", "hr", "main",
    "header", "footer", "article", "figure",
};

// Turns collected inline text into one paragraph.
std::string mdParagraph(const std::string& buf) {
    std::string t = tidy(buf);
    std::string out;
    std::vector<std::string> lines = splitLines(t);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += "\n";
        out += mdEscapeLineStart(lines[i]);
    }
    // Hard line breaks: two spaces and a newline.
    std::string r;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == kBr) {
            while (!r.empty() && r.back() == ' ') r.pop_back();
            size_t j = i + 1;
            while (j < out.size() && out[j] == ' ') ++j;
            if (!r.empty() && j < out.size()) r += "  \n";
            i = j - 1;
        } else {
            r += out[i];
        }
    }
    return jsTrim(r);
}

std::vector<std::string> mdBlocks(xmlNodePtr el) {
    std::vector<std::string> out;
    std::string buf;
    auto flush = [&] {
        std::string t = mdParagraph(buf);
        if (!t.empty()) out.push_back(t);
        buf.clear();
    };
    for (xmlNodePtr c = el->children; c; c = c->next) {
        if (isElement(c) && kMdBlock.count(nameOf(c))) {
            flush();
            for (auto& b : mdBlock(c)) out.push_back(b);
        } else {
            buf += mdInline(c);
        }
    }
    flush();
    return out;
}

std::string cellText(xmlNodePtr cell) {
    std::string t = collapseWs(textOf(cell));
    std::string out;
    for (char c : t) {
        if (c == '|') out += "\\|";
        else out += c;
    }
    return out;
}

std::vector<std::string> mdBlock(xmlNodePtr e) {
    const std::string tag = nameOf(e);
    if (isHeading(e)) {
        std::string t = collapseWs(joinStr(mdBlocks(e), " "));
        if (t.empty()) return {};
        return {std::string(static_cast<size_t>(headingLevel(e)), '#') + " " + t};
    }
    if (tag == "p" && attrOf(e, "class") == "img-alt") {
        std::string t = collapseWs(textOf(e));
        return t.empty() ? std::vector<std::string>{} : std::vector<std::string>{"*[Image: " + mdEscape(t) + "]*"};
    }
    if (tag == "ul" || tag == "ol") {
        const bool ordered = tag == "ol";
        int n = 1;
        std::vector<std::string> items;
        for (xmlNodePtr li : childElements(e)) {
            std::string content = joinStr(nameOf(li) == "li" ? mdBlocks(li) : mdBlock(li), "\n");
            if (content.empty()) continue;
            std::string marker = ordered ? std::to_string(n++) + ". " : "- ";
            items.push_back(prefixLines(content, marker, std::string(marker.size(), ' ')));
        }
        if (items.empty()) return {};
        return {joinStr(items, "\n")};
    }
    if (tag == "blockquote") {
        std::string inner = joinStr(mdBlocks(e), "\n\n");
        if (inner.empty()) return {};
        std::vector<std::string> lines = splitLines(prefixLines(inner, "> ", "> "));
        for (auto& l : lines)
            if (l == "> ") l = ">";
        return {joinStr(lines, "\n")};
    }
    if (tag == "pre") {
        std::string text = textOf(e);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        size_t run = 0, best = 0;
        for (char c : text) {
            run = c == '`' ? run + 1 : 0;
            best = std::max(best, run);
        }
        std::string fence(std::max<size_t>(3, best + 1), '`');
        return {fence + "\n" + text + "\n" + fence};
    }
    if (tag == "hr") return {"---"};
    if (tag == "table") {
        auto rows = descendants(e, [](xmlNodePtr x) { return nameOf(x) == "tr"; });
        if (rows.empty()) return {};
        std::vector<std::vector<std::string>> grid;
        size_t cols = 1;
        for (xmlNodePtr r : rows) {
            std::vector<std::string> cells;
            for (xmlNodePtr c : childElements(r)) cells.push_back(cellText(c));
            cols = std::max(cols, cells.size());
            grid.push_back(cells);
        }
        auto line = [&](const std::vector<std::string>& cells) {
            std::string s = "|";
            for (size_t i = 0; i < cols; ++i) s += " " + (i < cells.size() ? cells[i] : std::string()) + " |";
            return s;
        };
        std::vector<std::string> out = {line(grid[0]), "|" + repeatStr(" --- |", cols)};
        for (size_t i = 1; i < grid.size(); ++i) out.push_back(line(grid[i]));
        std::string capText;
        auto caps = descendants(e, [](xmlNodePtr x) { return nameOf(x) == "caption"; });
        if (!caps.empty()) capText = "*" + collapseWs(textOf(caps[0])) + "*\n\n";
        return {capText + joinStr(out, "\n")};
    }
    if (tag == "dl") {
        std::vector<std::string> lines;
        for (xmlNodePtr c : childElements(e)) {
            std::string t = joinStr(mdBlocks(c), "\n");
            if (t.empty()) continue;
            lines.push_back(nameOf(c) == "dt" ? "**" + t + "**" : t);
        }
        if (lines.empty()) return {};
        return {joinStr(lines, "\n")};
    }
    return mdBlocks(e);
}

} // namespace

std::string htmlToText(xmlNodePtr root, bool keepLinks, bool includeImages) {
    TxtOpts o{keepLinks, includeImages};
    return joinStr(txtBlocks(root, o), "\n\n");
}

std::string htmlToMarkdown(xmlNodePtr root) { return joinStr(mdBlocks(root), "\n\n"); }

} // namespace ks::output::detail
