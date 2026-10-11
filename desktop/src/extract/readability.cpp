// k-rosspointSender desktop: extract module, main-content finder.
// A small Readability-style scorer: paragraph density, comma count, link
// density and class/id hints. It picks the best container and adds good
// siblings. It prefers <article> and <main> when they hold enough text.
#include "extract/internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <regex>

namespace ks::extract::detail {

namespace {

size_t textLen(Node n) { return u8len(normText(textContent(n))); }

// Share of the text that is inside links. 0 when there is no text.
double linkDensity(Node n) {
    size_t total = textLen(n);
    if (!total) return 0;
    double linkLen = 0;
    for (Node a : querySelectorAll(n, "a")) {
        std::string href = attr(a, "href");
        double coeff = (!href.empty() && href[0] == '#') ? 0.3 : 1.0;
        linkLen += static_cast<double>(textLen(a)) * coeff;
    }
    return std::min(1.0, linkLen / static_cast<double>(total));
}

int countCommas(const std::string& s) {
    int n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ',') ++n;
        else if (s.compare(i, 2, "\xD8\x8C") == 0) ++n;        // U+060C
        else if (s.compare(i, 3, "\xEF\xBC\x8C") == 0) ++n;    // U+FF0C
    }
    return n;
}

// +25 for a content hint, -25 for a junk hint. Class and id count apart.
double classWeight(Node n) {
    static const std::regex positive("article|content|main|post|entry|body|text|story|blog", std::regex::icase);
    double w = 0;
    auto weigh = [&](const std::vector<std::string>& tokens, const std::string& whole) {
        if (whole.empty()) return;
        for (const auto& t : tokens)
            if (isJunkToken(t)) { w -= 25; break; }
        if (std::regex_search(whole, positive)) w += 25;
    };
    weigh(classList(n), attr(n, "class"));
    std::string id = attr(n, "id");
    weigh(id.empty() ? std::vector<std::string>{} : std::vector<std::string>{id}, id);
    return w;
}

double tagBase(const std::string& tag) {
    if (tag == "div") return 5;
    if (tag == "pre" || tag == "td" || tag == "blockquote") return 3;
    if (tag == "address" || tag == "ol" || tag == "ul" || tag == "dl" || tag == "dd" || tag == "dt" || tag == "li" || tag == "form") return -3;
    if (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4" || tag == "h5" || tag == "h6" || tag == "th") return -5;
    return 0;
}

int wordCount(const std::string& s) { return static_cast<int>(splitWords(s).size()); }

// Readability's title rule: cut the site name after a separator.
std::string readerTitle(Dom& dom) {
    Node t = querySelector(dom.root(), "title");
    std::string orig = t ? normText(textContent(t)) : "";
    std::string cur = orig;
    bool hierarchical = false;
    static const char* seps[] = {" | ", " - ", " \xE2\x80\x93 ", " \xE2\x80\x94 ", " \\ ", " / ", " > ", " \xC2\xBB "};
    size_t lastPos = std::string::npos, lastLen = 0, firstPos = std::string::npos, firstLen = 0;
    for (const char* sep : seps) {
        std::string s(sep);
        size_t p = orig.rfind(s);
        if (p != std::string::npos && (lastPos == std::string::npos || p > lastPos)) { lastPos = p; lastLen = s.size(); }
        size_t f = orig.find(s);
        if (f != std::string::npos && (firstPos == std::string::npos || f < firstPos)) { firstPos = f; firstLen = s.size(); }
        if (p != std::string::npos && (s == " \\ " || s == " / " || s == " > " || s == " \xC2\xBB ")) hierarchical = true;
    }
    if (lastPos != std::string::npos) {
        cur = orig.substr(0, lastPos);
        if (wordCount(cur) < 3) cur = orig.substr(firstPos + firstLen);
    } else if (u8len(orig) > 150 || u8len(orig) < 15) {
        auto h1s = querySelectorAll(dom.body(), "h1");
        if (h1s.size() == 1) cur = textContent(h1s[0]);
    }
    cur = normText(cur);
    (void)lastLen;
    if (wordCount(cur) <= 4) {
        std::string stripped = orig;
        for (const char* sep : seps) {
            std::string s(sep);
            size_t p;
            while ((p = stripped.find(s)) != std::string::npos) stripped.replace(p, s.size(), " ");
        }
        if (!hierarchical || wordCount(cur) != wordCount(stripped) - 1) cur = orig;
    }
    return cur;
}

bool isByline(Node n) {
    static const std::regex bylineRe("byline|author|dateline|writtenby|p-author", std::regex::icase);
    std::string match = attr(n, "class") + " " + attr(n, "id");
    bool hint = attr(n, "rel") == "author" || attr(n, "itemprop").find("author") != std::string::npos || std::regex_search(match, bylineRe);
    if (!hint) return false;
    size_t len = textLen(n);
    return len > 0 && len < 100;
}

bool hasBlockChild(Node n) {
    for (Node c : elementChildren(n))
        if (isBlock(c)) return true;
    return false;
}

} // namespace

double textSimilarity(const std::string& a, const std::string& b) {
    auto tokens = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : toLower(s)) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                cur += c;
            } else if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    auto ta = tokens(a), tb = tokens(b);
    if (ta.empty() || tb.empty()) return 0;
    auto joinLen = [](const std::vector<std::string>& v) {
        size_t n = 0;
        for (const auto& s : v) n += s.size();
        return v.empty() ? 0 : n + v.size() - 1;
    };
    std::vector<std::string> uniqB;
    for (const auto& t : tb)
        if (std::find(ta.begin(), ta.end(), t) == ta.end()) uniqB.push_back(t);
    return 1.0 - static_cast<double>(joinLen(uniqB)) / static_cast<double>(joinLen(tb));
}

ReaderResult readability(Dom& dom, const std::string& titleHint) {
    ReaderResult res;
    Node body = dom.body();
    res.title = readerTitle(dom);

    // 1. Find the byline element.
    Node bylineNode = nullptr;
    for (Node el : descendants(body)) {
        if (isByline(el)) {
            bylineNode = el;
            res.byline = normText(textContent(el));
            break;
        }
    }

    // 2. Score the paragraphs into their ancestors.
    std::map<Node, double> scores;
    std::vector<Node> order;
    auto initNode = [&](Node n) {
        if (scores.count(n)) return;
        scores[n] = tagBase(tagName(n)) + classWeight(n);
        order.push_back(n);
    };
    for (Node el : descendants(body)) {
        std::string tag = tagName(el);
        bool eligible = tag == "p" || tag == "pre" || tag == "td" || tag == "blockquote";
        if (!eligible && (tag == "div" || tag == "section") && !hasBlockChild(el)) eligible = true;
        if (!eligible) continue;
        if (bylineNode && contains(bylineNode, el)) continue;
        std::string text = normText(textContent(el));
        size_t len = u8len(text);
        if (len < 25) continue;
        std::vector<Node> ancestors;
        for (Node p = parentElement(el); p && ancestors.size() < 5; p = parentElement(p)) {
            if (isTag(p, "html")) break;
            ancestors.push_back(p);
        }
        if (ancestors.empty()) continue;
        double score = 1 + countCommas(text) + std::min(std::floor(static_cast<double>(len) / 100.0), 3.0);
        for (size_t level = 0; level < ancestors.size(); ++level) {
            Node a = ancestors[level];
            initNode(a);
            double divider = level == 0 ? 1 : level == 1 ? 2 : static_cast<double>(level) * 3;
            scores[a] += score / divider;
        }
    }

    // 3. Scale by link density. Drop link lists.
    std::map<Node, double> finalScores;
    Node top = nullptr;
    for (Node n : order) {
        double ld = linkDensity(n);
        if (ld > 0.5) continue;
        double s = scores[n] * (1 - ld);
        finalScores[n] = s;
        if (!top || s > finalScores[top]) top = n;
    }
    if (!top) return res;

    // 4. Walk up: a lone child gives way to its parent; a better parent wins.
    while (parentElement(top) && parentElement(top) != body && elementChildren(parentElement(top)).size() == 1 && !isTag(parentElement(top), "html"))
        top = parentElement(top);
    {
        double lastScore = finalScores.count(top) ? finalScores[top] : 0;
        double threshold = lastScore / 3;
        for (Node p = parentElement(top); p && p != body; p = parentElement(p)) {
            auto it = finalScores.find(p);
            if (it == finalScores.end()) continue;
            if (it->second < threshold) break;
            if (it->second > lastScore) {
                top = p;
                break;
            }
            lastScore = it->second;
        }
    }
    double topScore = finalScores.count(top) ? finalScores[top] : 0;

    // 5. Prefer <article> or <main> when it holds enough text.
    std::vector<Node> chosen;
    size_t bodyLen = textLen(body);
    for (const char* tag : {"article", "main"}) {
        Node best = nullptr;
        size_t bestLen = 0;
        for (Node el : querySelectorAll(body, tag)) {
            if (linkDensity(el) > 0.5) continue;
            size_t len = textLen(el);
            if (len > bestLen) {
                best = el;
                bestLen = len;
            }
        }
        if (best && bestLen >= 250 && (contains(best, top) || static_cast<double>(bestLen) >= 0.5 * static_cast<double>(bodyLen))) {
            chosen.push_back(best);
            break;
        }
    }

    // 6. Otherwise take the top candidate and its good siblings.
    if (chosen.empty()) {
        double siblingThreshold = std::max(10.0, topScore * 0.2);
        Node parent = parentElement(top);
        if (!parent) {
            chosen.push_back(top);
        } else {
            for (Node sib : elementChildren(parent)) {
                bool append = sib == top;
                if (!append) {
                    auto it = finalScores.find(sib);
                    if (it != finalScores.end() && it->second >= siblingThreshold) append = true;
                }
                if (!append && isTag(sib, "p")) {
                    std::string text = normText(textContent(sib));
                    size_t len = u8len(text);
                    double ld = linkDensity(sib);
                    if (len > 80 && ld < 0.25) append = true;
                    else if (len > 0 && len <= 80 && ld == 0 && (text.find(". ") != std::string::npos || text.back() == '.')) append = true;
                }
                if (append) chosen.push_back(sib);
            }
        }
    }

    // 7. Remove the byline and headings that repeat the title.
    if (bylineNode) {
        for (Node c : chosen) {
            if (c != bylineNode && contains(c, bylineNode)) {
                dom.remove(bylineNode);
                break;
            }
        }
    }
    for (Node c : chosen) {
        std::vector<Node> heads = querySelectorAll(c, "h1, h2");
        for (Node h : heads) {
            std::string ht = normText(textContent(h));
            if (ht.empty()) continue;
            if ((!res.title.empty() && textSimilarity(res.title, ht) > 0.75) || (!titleHint.empty() && textSimilarity(titleHint, ht) > 0.75))
                dom.remove(h);
        }
    }

    for (Node c : chosen) {
        res.contentHtml += outerHtml(c);
        std::string t = normText(textContent(c));
        if (!t.empty()) res.textContent += (res.textContent.empty() ? "" : " ") + t;
    }
    res.ok = !res.contentHtml.empty();
    return res;
}

} // namespace ks::extract::detail
