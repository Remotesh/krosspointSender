// K-rosspointSender desktop: extract module, Markdown and plain text to
// XHTML, word count and plain-text output. Port of src/extractors/text.ts
// and countWords in toc.ts.
#include "extract/internal.h"

#include <md4c-html.h>

#include <algorithm>
#include <set>
#include <sstream>

namespace ks::extract {

using namespace detail;

static void mdOut(const MD_CHAR* text, MD_SIZE size, void* user) {
    static_cast<std::string*>(user)->append(text, size);
}

std::string markdownToHtml(const std::string& md) {
    std::string raw;
    md_html(md.data(), static_cast<MD_SIZE>(md.size()), mdOut, &raw, MD_DIALECT_GITHUB, MD_HTML_FLAG_XHTML);
    return sanitizeHtml(raw, ExtractOptions{}, "");
}

std::string textToHtml(const std::string& txt) {
    // Normalize: no BOM, LF line ends, tabs as four spaces.
    std::string norm;
    size_t start = txt.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    for (size_t i = start; i < txt.size(); ++i) {
        char c = txt[i];
        if (c == '\r') {
            norm += '\n';
            if (i + 1 < txt.size() && txt[i + 1] == '\n') ++i;
        } else if (c == '\t') {
            norm += "    ";
        } else {
            norm += c;
        }
    }
    // A blank line starts a new paragraph.
    std::vector<std::vector<std::string>> paras(1);
    std::stringstream ss(norm);
    std::string line;
    while (std::getline(ss, line)) {
        if (normText(line).empty()) {
            if (!paras.back().empty()) paras.emplace_back();
            continue;
        }
        paras.back().push_back(rtrimSpace(line));
    }
    std::string out;
    for (const auto& lines : paras) {
        if (lines.empty()) continue;
        size_t total = 0;
        for (const auto& l : lines) total += u8len(l);
        double avg = static_cast<double>(total) / static_cast<double>(lines.size());
        bool hardWrapped = lines.size() > 1 && avg >= 50;
        std::string joined;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) joined += hardWrapped ? " " : "<br/>";
            joined += escapeHtml(cleanXmlText(trim(lines[i])));
        }
        if (!out.empty()) out += "\n";
        out += "<p>" + joined + "</p>";
    }
    return out;
}

namespace detail {

static void wordText(Node n, std::string& out) {
    static const std::set<std::string> blocks = {"p", "li", "h1", "h2", "h3", "h4", "h5", "h6", "td", "th", "br", "div",
                                                 "blockquote", "figcaption", "dt", "dd", "pre", "caption"};
    for (Node c = n->children; c; c = c->next) {
        if (isText(c)) {
            if (c->content) out += reinterpret_cast<const char*>(c->content);
        } else if (isElement(c)) {
            wordText(c, out);
            if (blocks.count(tagName(c))) out += ' ';
        }
    }
}

int countWordsNode(Node root) {
    std::string text;
    wordText(root, text);
    return static_cast<int>(splitWords(text).size());
}

} // namespace detail

int countWords(const std::string& html) {
    if (trim(html).empty()) return 0;
    auto dom = parseFragment(html);
    return countWordsNode(dom->body());
}

namespace {

struct TextWriter {
    std::string out;
    int pendingBreaks = 0;   // newlines to write before the next text
    bool atLineStart = true;

    void breakLines(int n) {
        if (out.empty()) return;
        pendingBreaks = std::max(pendingBreaks, n);
    }
    void flushBreaks() {
        if (pendingBreaks && !out.empty()) {
            out.append(static_cast<size_t>(pendingBreaks), '\n');
            atLineStart = true;
        }
        pendingBreaks = 0;
    }
    void text(const std::string& raw, bool pre) {
        if (pre) {
            if (raw.empty()) return;
            flushBreaks();
            out += raw;
            atLineStart = !raw.empty() && raw.back() == '\n';
            return;
        }
        std::string t = normText(raw);
        bool leadSpace = !raw.empty() && t.size() != raw.size() && (raw[0] == ' ' || raw[0] == '\n' || raw[0] == '\t' || raw[0] == '\r');
        bool trailSpace = !raw.empty() && (raw.back() == ' ' || raw.back() == '\n' || raw.back() == '\t' || raw.back() == '\r');
        if (t.empty()) {
            if (!raw.empty() && !atLineStart && !pendingBreaks && !out.empty() && out.back() != ' ') out += ' ';
            return;
        }
        if (pendingBreaks) flushBreaks();
        else if (leadSpace && !atLineStart && !out.empty() && out.back() != ' ') out += ' ';
        out += t;
        if (trailSpace) out += ' ';
        atLineStart = false;
    }
};

void walkText(Node n, TextWriter& w, bool pre) {
    static const std::set<std::string> paraBlocks = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote", "pre", "figure", "table",
                                                     "ul", "ol", "dl", "hr", "div", "section", "article", "main", "header", "footer", "aside"};
    static const std::set<std::string> lineBlocks = {"li", "tr", "dt", "dd", "figcaption", "caption", "address"};
    for (Node c = n->children; c; c = c->next) {
        if (isText(c)) {
            w.text(c->content ? reinterpret_cast<const char*>(c->content) : "", pre);
            continue;
        }
        if (!isElement(c)) continue;
        std::string tag = tagName(c);
        if (tag == "script" || tag == "style" || tag == "head" || tag == "template") continue;
        if (tag == "br") {
            w.breakLines(1);
            continue;
        }
        if (tag == "td" || tag == "th") {
            if (c->prev && !w.atLineStart && !w.out.empty()) w.out += '\t';
        }
        bool para = paraBlocks.count(tag) > 0;
        bool line = lineBlocks.count(tag) > 0;
        if (para) w.breakLines(2);
        else if (line) w.breakLines(1);
        walkText(c, w, pre || tag == "pre");
        if (para) w.breakLines(2);
        else if (line) w.breakLines(1);
    }
}

} // namespace

std::string htmlToText(const std::string& html) {
    if (trim(html).empty()) return "";
    auto dom = parseFragment(html);
    TextWriter w;
    walkText(dom->body(), w, false);
    // Trim trailing spaces on each line, and the whole text.
    std::string out;
    std::stringstream ss(w.out);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        while (!line.empty() && line.back() == ' ') line.pop_back();
        if (!first) out += '\n';
        out += line;
        first = false;
    }
    return trim(out);
}

} // namespace ks::extract
