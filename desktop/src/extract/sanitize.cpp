// K-rosspointSender desktop: extract module, junk removal, layout clean-up
// and the allowlist sanitizer. Port of src/extractors/dom.ts and sanitize.ts.
#include "extract/internal.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>

namespace ks::extract::detail {

// ============================================================= junk removal

// Tags that never carry article text. We remove them with their content.
static const char* kJunkTags =
    "script, style, link, meta, template, noscript, iframe, frame, frameset, object, embed, form, button, input, select, "
    "textarea, label, dialog, canvas, svg, video, audio, nav, header, footer, aside, menu";

// Roles that mark page chrome, not content.
static const std::set<std::string> kJunkRoles = {"navigation", "banner", "contentinfo", "complementary", "search", "dialog",
                                                 "alertdialog", "menu", "menubar", "toolbar"};

// Class and id words that mark junk. A word matches a whole token part
// between "-" or "_". Example: "share" matches "share-bar", not "shared".
static const std::set<std::string> kStrongWords = {
    // ads and sponsors
    "ad", "ads", "adv", "advert", "adverts", "advertisement", "advertising", "adsbygoogle", "adslot", "adunit", "adbox",
    "adcontainer", "adwrapper", "dfp", "gpt", "sponsor", "sponsored", "sponsors", "promo", "promoted", "promotion",
    "outbrain", "taboola", "revcontent", "zergnet", "mgid", "teads", "affiliate",
    // cookie and consent
    "cookie", "cookies", "consent", "gdpr", "cmp", "onetrust", "cookiebanner", "cookiebar", "didomi",
    // comments
    "comment", "comments", "commentlist", "disqus", "respond", "replies", "discussion", "coral",
    // related and recommended
    "related", "recommended", "recommendations", "readmore", "popular", "trending", "upnext",
    // share and social
    "share", "sharing", "sharebar", "sharetools", "social", "socials", "sociallinks", "addthis", "sharethis",
    // navigation and chrome
    "nav", "navbar", "navigation", "menu", "breadcrumb", "breadcrumbs", "sidebar", "masthead", "topbar", "toolbar",
    "skiplink", "pagination", "pager", "header", "footer", "sitefooter", "siteheader",
    // popups and forms
    "newsletter", "subscribe", "subscription", "signup", "paywall", "regwall", "popup", "modal", "overlay", "banner",
    "lightbox", "toast",
    // misc widgets
    "widget", "screenreader", "tagcloud"};

// Weak words match only in combined tokens like "read-more".
static const std::vector<std::regex>& weakCombos() {
    static const std::vector<std::regex> v = {
        std::regex(R"(^read[-_]?more)"), std::regex(R"(more[-_](stories|articles|from|news|like))"),
        std::regex(R"(most[-_](read|popular|viewed))"), std::regex(R"((^|[-_])also[-_]read)"),
        std::regex(R"(^(next|prev)[-_](post|article|story))"), std::regex(R"(^site[-_]?(header|footer|nav|menu))"),
        std::regex(R"(^print[-_]?(only|button))"), std::regex(R"(^(sr|screen[-_]?reader|visually)[-_]?(only|hidden))"),
        std::regex(R"(^skip[-_]?(link|to))"), std::regex(R"(^follow[-_]?(us|button))"),
        std::regex(R"(^(login|register)[-_]?(form|wall|prompt))"), std::regex(R"(^tags?[-_]?(list|cloud|links))"),
        std::regex(R"(privacy[-_]?(banner|notice|popup))"),
    };
    return v;
}

bool isJunkToken(const std::string& token) {
    std::string t = toLower(token);
    if (t.empty()) return false;
    static const std::set<std::string> contentWords = {"article", "entry", "post", "story", "content", "main", "body", "text"};
    if (contentWords.count(t)) return false;
    static const std::regex headerWord(R"((^|[-_])(header|heading|headline|title)([-_]|$))");
    static const std::regex contentHeader(R"((article|entry|post|story|content|page)[-_]?(header|heading))");
    if (std::regex_search(t, headerWord) && std::regex_search(t, contentHeader)) return false;
    // Split into parts on "-" and "_".
    std::vector<std::string> parts;
    std::string cur;
    for (char c : t) {
        if (c == '-' || c == '_') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (kStrongWords.count(parts[i])) return true;
        if (parts[i] == "authorbio") return true;
        if (parts[i] == "author" && i + 1 < parts.size() && parts[i + 1] == "bio") return true;
    }
    for (const auto& re : weakCombos())
        if (std::regex_search(t, re)) return true;
    return false;
}

static void removeComments(Dom& dom, Node n) {
    Node c = n->children;
    while (c) {
        Node next = c->next;
        if (c->type == XML_COMMENT_NODE) dom.remove(c);
        else if (isElement(c)) removeComments(dom, c);
        c = next;
    }
}

void removeJunk(Dom& dom, Node root, bool patterns) {
    static const Selector junkTags(kJunkTags);
    for (Node el : querySelectorAll(root, junkTags))
        if (isConnected(el)) dom.remove(el);
    for (Node el : descendants(root)) {
        if (!isConnected(el)) continue;
        bool junk = false;
        if (hasAttr(el, "role") && kJunkRoles.count(attr(el, "role"))) junk = true;
        if (hasAttr(el, "hidden")) junk = true;
        if (attr(el, "aria-hidden") == "true" && !isTag(el, "img")) junk = true;
        std::string style = attr(el, "style");
        if (style.find("display:none") != std::string::npos || style.find("display: none") != std::string::npos) junk = true;
        if (junk) dom.remove(el);
    }
    removeComments(dom, root);
    if (!patterns) return;

    size_t totalText = u8len(normText(textContent(root)));
    if (!totalText) totalText = 1;
    static const Selector protectedInside("article, main, [itemprop=\"articleBody\"]");
    for (Node el : querySelectorAll(root, "[class], [id]")) {
        if (!isConnected(el)) continue;
        std::string tag = tagName(el);
        if (tag == "html" || tag == "body" || tag == "main" || tag == "article") continue;
        std::vector<std::string> tokens = classList(el);
        std::string id = attr(el, "id");
        if (!id.empty()) tokens.push_back(id);
        bool junk = false;
        for (const auto& t : tokens)
            if (isJunkToken(t)) { junk = true; break; }
        if (!junk) continue;
        if (querySelector(el, protectedInside)) continue;
        if (isHeading(el)) continue;
        size_t len = u8len(normText(textContent(el)));
        if (len > totalText * 0.4 && len > 500) continue;
        dom.remove(el);
    }
}

// ============================================================= images and figures

// Parse a number like JavaScript Number(): the whole string must be numeric.
static double jsNumber(const std::string& s) {
    std::string t = trim(s);
    if (t.empty()) return 0;
    char* end = nullptr;
    double v = std::strtod(t.c_str(), &end);
    if (!end || *end != '\0') return NAN;
    return v;
}

// Pick the largest candidate from a srcset. Fall back to the last entry.
static std::string pickFromSrcset(const std::string& srcset, bool preferLast = false) {
    if (srcset.empty()) return "";
    struct Entry { std::string url; double size; };
    std::vector<Entry> entries;
    // Split on a comma that has whitespace after it.
    std::vector<std::string> pieces;
    size_t start = 0;
    for (size_t i = 0; i < srcset.size(); ++i) {
        if (srcset[i] == ',' && i + 1 < srcset.size() && std::isspace(static_cast<unsigned char>(srcset[i + 1]))) {
            size_t j = i + 1;
            while (j < srcset.size() && std::isspace(static_cast<unsigned char>(srcset[j]))) ++j;
            if (j < srcset.size()) {
                pieces.push_back(srcset.substr(start, i - start));
                start = j;
                i = j - 1;
            }
        }
    }
    pieces.push_back(srcset.substr(start));
    static const std::regex desc(R"(^([\d.]+)([wx])$)");
    for (const auto& p : pieces) {
        auto words = splitWords(p);
        if (words.empty()) continue;
        double size = 0;
        std::smatch m;
        if (words.size() > 1 && std::regex_match(words[1], m, desc)) size = std::atof(m[1].str().c_str()) * (m[2] == "x" ? 1000 : 1);
        entries.push_back({words[0], size});
    }
    if (entries.empty()) return "";
    if (preferLast) return entries.back().url;
    Entry best = entries.back();
    for (const auto& e : entries)
        if (e.size > best.size) best = e;
    return best.url;
}

static bool isPlaceholderSrc(const std::string& src) {
    std::string s = trim(src);
    if (s.empty()) return true;
    static const std::regex dataGif(R"(^data:image/(gif|svg))", std::regex::icase);
    if (std::regex_search(s, dataGif) && s.size() < 400) return true;
    if (startsWithNoCase(s, "data:")) return false;
    static const std::regex ph(R"((blank|spacer|placeholder|transparent|pixel|lazy)[^/]*\.(gif|png|svg))", std::regex::icase);
    return std::regex_search(s, ph);
}

void fixImages(Dom& dom, Node root, bool preferSrcset) {
    static const char* lazyAttrs[] = {"data-src", "data-lazy-src", "data-original", "data-orig-file", "data-url", "data-hi-res-src", "data-full-src", "data-lazyload"};
    static const std::regex modernType("webp|avif", std::regex::icase);
    for (Node pic : querySelectorAll(root, "picture")) {
        if (!isConnected(pic)) continue;
        Node img = querySelector(pic, "img");
        if (!img) {
            dom.remove(pic);
            continue;
        }
        if (isPlaceholderSrc(attr(img, "src"))) {
            auto sources = querySelectorAll(pic, "source");
            Node source = nullptr;
            for (Node s : sources)
                if (!std::regex_search(attr(s, "type"), modernType)) { source = s; break; }
            if (!source && !sources.empty()) source = sources[0];
            if (source) {
                std::string ss = hasAttr(source, "srcset") ? attr(source, "srcset") : attr(source, "data-srcset");
                std::string cand = pickFromSrcset(ss);
                if (!cand.empty()) setAttr(img, "src", cand);
            }
        }
        dom.replaceWith(pic, img);
    }
    for (Node img : querySelectorAll(root, "img")) {
        if (!isConnected(img)) continue;
        std::string src = attr(img, "src");
        if (isPlaceholderSrc(src)) {
            std::string lazy;
            for (const char* a : lazyAttrs) {
                std::string v = attr(img, a);
                if (!v.empty() && !isPlaceholderSrc(v)) { lazy = v; break; }
            }
            if (!lazy.empty()) {
                src = lazy;
            } else {
                std::string ss = hasAttr(img, "data-srcset") ? attr(img, "data-srcset") : attr(img, "srcset");
                std::string cand = pickFromSrcset(ss);
                if (!cand.empty()) src = cand;
            }
        } else if (preferSrcset) {
            std::string cand = pickFromSrcset(attr(img, "srcset"), true);
            if (!cand.empty()) src = cand;
        }
        if (src.empty() || isPlaceholderSrc(src)) {
            dom.remove(img);
            continue;
        }
        setAttr(img, "src", src);
        removeAttr(img, "srcset");
        removeAttr(img, "data-srcset");
        // Tracking pixels.
        double w = jsNumber(attr(img, "width"));
        double h = jsNumber(attr(img, "height"));
        if ((w > 0 && w <= 2) || (h > 0 && h <= 2)) dom.remove(img);
    }
}

static Node buildFigure(Dom& dom, const std::vector<Node>& imgs, Node captionNode) {
    Node fig = dom.createElement("figure");
    for (Node img : imgs) {
        Node clean = dom.createElement("img");
        setAttr(clean, "src", attr(img, "src"));
        setAttr(clean, "alt", attr(img, "alt"));
        appendChild(fig, clean);
    }
    if (captionNode && !normText(textContent(captionNode)).empty()) {
        Node cap = dom.createElement("figcaption");
        for (Node c = captionNode->children; c; c = c->next) appendChild(cap, dom.copyNode(c));
        // A caption holds inline content only.
        for (Node b : querySelectorAll(cap, "div, p, figure, img")) {
            if (!contains(cap, b)) continue;
            if (isTag(b, "img") || isTag(b, "figure")) dom.remove(b);
            else dom.unwrap(b);
        }
        appendChild(fig, cap);
    }
    return fig;
}

// True when a <p> inside el has an earlier sibling <p> ("p ~ p").
static bool hasSiblingParagraphs(Node el) {
    for (Node p : querySelectorAll(el, "p"))
        for (Node s = p->prev; s; s = s->prev)
            if (isTag(s, "p")) return true;
    return false;
}

void normalizeFigures(Dom& dom, Node root) {
    for (Node fig : querySelectorAll(root, "figure")) {
        if (!isConnected(fig)) continue;
        auto imgs = querySelectorAll(fig, "img");
        if (imgs.empty()) {
            // A figure without an image (quote, code). Keep the content.
            if (normText(textContent(fig)).empty()) dom.remove(fig);
            else if (querySelector(fig, "pre, blockquote, table")) dom.unwrap(fig);
            continue;
        }
        Node cap = querySelector(fig, "figcaption");
        if (!cap) cap = querySelector(fig, "[class*=\"caption\"]");
        dom.replaceWith(fig, buildFigure(dom, imgs, cap));
    }
    static const Selector capSel("figcaption, [class*=\"caption\"], [class*=\"Caption\"], .credit");
    static const Selector bigBlocks("h1, h2, h3, h4, h5, h6, ul, ol, table");
    for (Node el : querySelectorAll(root, "div, span, p, section")) {
        if (!isConnected(el)) continue;
        auto imgs = querySelectorAll(el, "img");
        if (imgs.size() != 1) continue;
        Node cap = querySelector(el, capSel);
        if (!cap) continue;
        // The wrapper must hold only the image, the caption, and little else.
        long capLen = static_cast<long>(u8len(normText(textContent(cap))));
        long restLen = static_cast<long>(u8len(normText(textContent(el)))) - capLen;
        if (restLen > 20 || hasSiblingParagraphs(el) || querySelector(el, bigBlocks)) continue;
        dom.replaceWith(el, buildFigure(dom, {imgs[0]}, cap));
    }
}

// ============================================================= linearize

bool isBlock(Node n) {
    static const std::set<std::string> blocks = {
        "p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "dl", "dt", "dd", "blockquote", "pre", "table", "figure",
        "figcaption", "hr", "div", "section", "article", "main", "header", "footer", "aside", "nav", "address", "details",
        "summary", "fieldset", "hgroup", "center", "thead", "tbody", "tfoot", "tr", "td", "th", "caption", "form"};
    return isElement(n) && blocks.count(tagName(n)) > 0;
}

static bool hasNonSpace(const std::string& s) { return !normText(s).empty(); }

static bool hasContent(const std::vector<Node>& nodes) {
    for (Node n : nodes) {
        if (isText(n) && hasNonSpace(reinterpret_cast<const char*>(n->content ? n->content : BAD_CAST ""))) return true;
        if (isElement(n) && (isTag(n, "img") || querySelector(n, "img") || hasNonSpace(textContent(n)))) return true;
    }
    return false;
}

void wrapInlineRuns(Dom& dom, Node parent) {
    std::vector<Node> run;
    auto flush = [&](Node before) {
        if (!run.empty() && hasContent(run)) {
            Node p = dom.createElement("p");
            insertBefore(parent, p, before);
            for (Node n : run) appendChild(p, n);
        } else {
            for (Node n : run)
                if (isText(n)) dom.remove(n);
        }
        run.clear();
    };
    for (Node child : childNodes(parent)) {
        if (isBlock(child)) flush(child);
        else if (isTag(child, "br") && run.empty()) dom.remove(child);
        else run.push_back(child);
    }
    flush(nullptr);
}

static void renameElement(Node el, const char* tag) {
    xmlNodeSetName(el, BAD_CAST tag);
    keepOnlyAttrs(el, {"id", "colspan", "rowspan"});
}

void linearize(Dom& dom, Node root) {
    auto list = querySelectorAll(root, "div, section, article, main, center, details, summary, address, fieldset, hgroup, header, footer, span[data-block]");
    static const std::set<std::string> inlineParents = {"p", "li", "td", "th", "dt", "dd", "figcaption", "caption", "h1", "h2", "h3",
                                                        "h4", "h5", "h6", "a", "span", "b", "strong", "i", "em"};
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        Node el = *it;
        if (!isConnected(el)) continue;
        auto kids = childNodes(el);
        bool anyBlock = false;
        for (Node k : kids)
            if (isBlock(k)) { anyBlock = true; break; }
        if (anyBlock) {
            wrapInlineRuns(dom, el);
            dom.unwrap(el);
        } else if (hasContent(kids)) {
            Node parent = parentElement(el);
            // Inside a <p>, <li>, cell or caption a <p> is not needed.
            if (parent && inlineParents.count(tagName(parent))) dom.unwrap(el);
            else renameElement(el, "p");
        } else {
            dom.remove(el);
        }
    }
}

// ============================================================= tables

void handleTables(Dom& dom, Node root, bool keepTables) {
    // Layout tables: a table that holds another table, or a single cell.
    for (int guard = 0; guard <= 200; ++guard) {
        Node outer = nullptr;
        for (Node t : querySelectorAll(root, "table")) {
            if (querySelector(t, "table") || querySelectorAll(t, "td, th").size() <= 1) {
                outer = t;
                break;
            }
        }
        if (!outer) break;
        Node box = dom.createElement("div");
        for (Node cap : elementChildren(outer)) {
            if (!isTag(cap, "caption")) continue;
            Node p = dom.createElement("p");
            while (cap->children) appendChild(p, cap->children);
            appendChild(box, p);
        }
        for (Node cell : querySelectorAll(outer, "td, th")) {
            if (closest(cell, "table") != outer) continue;
            Node d = dom.createElement("div");
            while (cell->children) appendChild(d, cell->children);
            appendChild(box, d);
        }
        dom.replaceWith(outer, box);
    }
    if (keepTables) return;
    for (Node table : querySelectorAll(root, "table")) {
        if (!isConnected(table)) continue;
        Node box = dom.createElement("div");
        Node cap = querySelector(table, "caption");
        if (cap && !normText(textContent(cap)).empty()) {
            Node p = dom.createElement("p");
            Node b = dom.createElement("strong");
            appendChild(b, dom.createText(normText(textContent(cap))));
            appendChild(p, b);
            appendChild(box, p);
        }
        for (Node tr : querySelectorAll(table, "tr")) {
            std::string line;
            for (Node c : querySelectorAll(tr, "td, th")) {
                std::string t = normText(textContent(c));
                if (t.empty()) continue;
                if (!line.empty()) line += " | ";
                line += t;
            }
            if (line.empty()) continue;
            Node p = dom.createElement("p");
            appendChild(p, dom.createText(line));
            appendChild(box, p);
        }
        dom.replaceWith(table, box);
    }
}

// ============================================================= allowlist

static const std::set<std::string>& allowedTags() {
    static const std::set<std::string> s = {"h1", "h2", "h3", "h4", "h5", "h6", "p", "br", "hr", "ul", "ol", "li", "blockquote", "pre",
                                            "code", "b", "strong", "i", "em", "u", "s", "del", "sub", "sup", "span", "a", "img",
                                            "figure", "figcaption", "table", "thead", "tbody", "tr", "th", "td", "caption", "dl", "dt", "dd"};
    return s;
}

static const std::set<std::string>& allowedAttrs() {
    static const std::set<std::string> s = {"href", "src", "alt", "id", "colspan", "rowspan"};
    return s;
}

// Elements that go with their content (DOMPurify FORBID_CONTENTS).
static const std::set<std::string>& forbidContents() {
    static const std::set<std::string> s = {"annotation-xml", "audio", "colgroup", "desc", "foreignobject", "head", "iframe", "math",
                                            "mi", "mn", "mo", "ms", "mtext", "noembed", "noframes", "noscript", "plaintext", "script",
                                            "style", "svg", "template", "title", "video", "xmp", "object", "embed", "select",
                                            "textarea", "button"};
    return s;
}

// Remove whitespace and control characters, then test for a bad scheme.
static bool hasBadScheme(const std::string& v, bool allowData) {
    std::string compact;
    for (unsigned char c : v)
        if (c > 0x20) compact += static_cast<char>(std::tolower(c));
    static const char* bad[] = {"javascript:", "vbscript:", "file:", "blob:"};
    for (const char* b : bad)
        if (compact.rfind(b, 0) == 0) return true;
    if (!allowData && compact.rfind("data:", 0) == 0) return true;
    return false;
}

static void applyAllowlist(Dom& dom, Node parent, const std::string& base) {
    for (Node c : childNodes(parent)) {
        if (isElement(c)) {
            std::string t = tagName(c);
            bool allowed = allowedTags().count(t) > 0;
            if (!allowed && forbidContents().count(t)) {
                dom.remove(c);
                continue;
            }
            applyAllowlist(dom, c, base);
            if (!allowed) {
                dom.unwrap(c);
                continue;
            }
            keepOnlyAttrs(c, allowedAttrs());
            if (hasAttr(c, "href")) {
                std::string h = trim(attr(c, "href"));
                if (h.empty() || hasBadScheme(h, false)) removeAttr(c, "href");
                else setAttr(c, "href", resolveUrl(h, base));
            }
            if (hasAttr(c, "src")) {
                std::string s = trim(attr(c, "src"));
                bool isImg = t == "img";
                if (startsWithNoCase(s, "data:")) {
                    if (!(isImg && startsWithNoCase(s, "data:image/"))) removeAttr(c, "src");
                } else if (s.empty() || hasBadScheme(s, true)) {
                    removeAttr(c, "src");
                } else {
                    setAttr(c, "src", resolveUrl(s, base));
                }
            }
        } else if (isText(c)) {
            if (c->type == XML_CDATA_SECTION_NODE) c->type = XML_TEXT_NODE;
        } else {
            dom.remove(c);
        }
    }
}

static bool isEdgeBr(Node br, bool forward) {
    static const std::set<std::string> blocks = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "dl", "table", "figure", "blockquote", "pre", "hr"};
    Node n = forward ? br->next : br->prev;
    while (n && isText(n) && !hasNonSpace(reinterpret_cast<const char*>(n->content ? n->content : BAD_CAST ""))) n = forward ? n->next : n->prev;
    return !n || (isElement(n) && blocks.count(tagName(n)));
}

std::string sanitizeHtmlImpl(const std::string& html, const ExtractOptions& opts, const std::string& baseUrl) {
    // 1. Structural clean-up before the allowlist.
    auto dom = parseFragment(html);
    Node body = dom->body();
    removeJunk(*dom, body, false);
    fixImages(*dom, body);
    normalizeFigures(*dom, body);
    handleTables(*dom, body, opts.keepTables);
    linearize(*dom, body);

    // 2. The allowlist.
    applyAllowlist(*dom, body, baseUrl);

    // 3. Clean-up after the allowlist.
    for (Node span : querySelectorAll(body, "span"))
        if (attr(span, "id").empty()) dom->unwrap(span);
    for (Node a : querySelectorAll(body, "a")) {
        if (!opts.keepLinks || attr(a, "href").empty()) {
            std::string id = attr(a, "id");
            // Keep the anchor target. Links to it still work.
            if (!id.empty()) {
                Node anchor = dom->createElement("span");
                setAttr(anchor, "id", id);
                insertBefore(a->parent, anchor, a);
            }
            dom->unwrap(a);
        }
    }
    for (Node img : querySelectorAll(body, "img")) {
        if (!opts.keepImages || attr(img, "src").empty()) dom->remove(img);
        else if (!hasAttr(img, "alt")) setAttr(img, "alt", "");
    }
    // Remove empty elements, deepest first.
    static const Selector emptyCheck("p, figure, figcaption, h1, h2, h3, h4, h5, h6, li, blockquote, b, strong, i, em, u, s, del, sub, sup, a, code, pre, caption, dt, dd, ul, ol, dl");
    static const Selector keepers("img, hr, table");
    auto list = querySelectorAll(body, emptyCheck);
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        Node el = *it;
        if (!isConnected(el)) continue;
        if (isTag(el, "a") && !attr(el, "id").empty()) continue;
        if (querySelector(el, keepers) || hasNonSpace(textContent(el))) continue;
        dom->remove(el);
    }
    for (Node fig : querySelectorAll(body, "figure"))
        if (!querySelector(fig, "img")) dom->unwrap(fig);
    // A figcaption outside a figure becomes a paragraph. A figcaption that
    // holds a block element becomes <div class="caption">.
    for (Node cap : querySelectorAll(body, "figcaption")) {
        if (!isTag(cap->parent, "figure")) {
            xmlNodeSetName(cap, BAD_CAST "p");
            continue;
        }
        for (Node d : descendants(cap)) {
            if (isBlock(d)) {
                xmlNodeSetName(cap, BAD_CAST "div");
                setAttr(cap, "class", "caption");
                break;
            }
        }
    }
    // Remove <br> at the start and the end of a block, and <br> between blocks.
    for (Node br : querySelectorAll(body, "br"))
        if (isEdgeBr(br, false) || isEdgeBr(br, true)) dom->remove(br);
    wrapInlineRuns(*dom, body);
    return trim(serializeXhtml(body));
}

} // namespace ks::extract::detail

namespace ks::extract {

std::string sanitizeHtml(const std::string& html, const ExtractOptions& opts, const std::string& baseUrl) {
    return detail::sanitizeHtmlImpl(html, opts, baseUrl);
}

} // namespace ks::extract
