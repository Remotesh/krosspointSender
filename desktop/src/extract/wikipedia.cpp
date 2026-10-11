// K-rosspointSender desktop: extract module, Wikipedia clean-up.
// It works on the desktop page, the mobile page and the action=parse output.
// Port of src/extractors/wikipedia.ts.
#include "extract/internal.h"

#include <cstdlib>
#include <regex>

namespace ks::extract {

using namespace detail;

namespace {

// Selectors that we always remove.
const char* kAlwaysRemove =
    ".navbox, .vertical-navbox, .sidebar, .mw-editsection, .hatnote, .ambox, #toc, .toc, .metadata, .catlinks, .mw-jump-link, "
    ".noprint, .portal, #coordinates, .mw-empty-elt, .shortdescription, .navbox-styles, .mw-authority-control, "
    ".authority-control, .sistersitebox, .side-box, .mbox-small, .mw-indicators, .printfooter, .mw-cite-backlink, style, "
    "link, meta, script, noscript";

// Reference markers and lists. keepReferences keeps them.
const char* kReferenceSelectors = ".reference, sup.reference, .mw-references-wrap, ol.references, .reflist";

const std::vector<std::string> kRefSections = {"references", "notes", "citations", "notes and references", "references and notes",
                                               "footnotes", "sources", "works cited"};
const std::vector<std::string> kOtherSections = {"external links", "see also", "further reading", "bibliography"};

int headingLevel(Node n) {
    if (!isElement(n)) return 0;
    if (int l = headingLevelOfTag(n)) return l;
    if (hasClass(n, "mw-heading")) {
        Node h = querySelector(n, "h1, h2, h3, h4, h5, h6");
        return h ? headingLevelOfTag(h) : 0;
    }
    if (isTag(n, "section")) {
        Node first = firstElementChild(n);
        return first ? headingLevel(first) : 0;
    }
    return 0;
}

// Text of a node without the [edit] links.
void textSkippingEdit(Node n, std::string& out) {
    for (Node c = n->children; c; c = c->next) {
        if (isText(c) && c->content) out += reinterpret_cast<const char*>(c->content);
        else if (isElement(c) && !hasClass(c, "mw-editsection")) textSkippingEdit(c, out);
    }
}

std::string removeEditMarks(const std::string& s) {
    static const std::regex edit(R"(\[\s*edit\s*\])", std::regex::icase);
    return std::regex_replace(s, edit, "");
}

std::string headingTitle(Node h) {
    std::string t;
    textSkippingEdit(h, t);
    return toLower(trim(removeEditMarks(normText(t))));
}

void removeSections(Dom& dom, Node root, const std::set<std::string>& titles) {
    for (Node h : querySelectorAll(root, "h2, h3, h4, h5, h6")) {
        if (!isConnected(h) || !titles.count(headingTitle(h))) continue;
        int level = headingLevelOfTag(h);
        Node unit = (h->parent && hasClass(h->parent, "mw-heading")) ? h->parent : h;
        Node sec = unit->parent;
        if (sec && isTag(sec, "section") && firstElementChild(sec) == unit) {
            dom.remove(sec);
            continue;
        }
        Node n = unit->next;
        dom.remove(unit);
        while (n) {
            int lvl = headingLevel(n);
            if (lvl && lvl <= level) break;
            Node next = n->next;
            dom.remove(n);
            n = next;
        }
    }
}

std::string absImageSrc(Node img, const std::string& base) {
    std::string last;
    std::string srcset = attr(img, "srcset");
    size_t start = 0;
    while (start <= srcset.size()) {
        size_t comma = srcset.find(',', start);
        std::string piece = srcset.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        auto words = splitWords(piece);
        if (!words.empty()) last = words[0];
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    std::string raw = !last.empty() ? last : !attr(img, "src").empty() ? attr(img, "src") : attr(img, "data-src");
    if (raw.empty()) return "";
    if (raw.rfind("//", 0) == 0) raw = "https:" + raw;
    std::string abs = resolveUrl(raw, base);
    if (startsWithNoCase(abs, "http://")) abs = "https://" + abs.substr(7);
    if (!startsWithNoCase(abs, "https://") && !startsWithNoCase(abs, "data:")) return "";
    return abs;
}

// Turn .thumb blocks, figure[typeof] and gallery boxes into figure/img/figcaption.
void convertFigures(Dom& dom, Node root, const std::string& base) {
    // Lazy image placeholders on the mobile site.
    for (Node ph : querySelectorAll(root, "span.lazy-image-placeholder[data-src]")) {
        Node img = dom.createElement("img");
        setAttr(img, "src", attr(ph, "data-src"));
        std::string ss = attr(ph, "data-srcset");
        if (!ss.empty()) setAttr(img, "srcset", ss);
        setAttr(img, "alt", attr(ph, "data-alt"));
        dom.replaceWith(ph, img);
    }
    for (Node img : querySelectorAll(root, "img")) {
        std::string src = absImageSrc(img, base);
        if (src.empty()) {
            dom.remove(img);
            continue;
        }
        setAttr(img, "src", src);
        removeAttr(img, "srcset");
        removeAttr(img, "data-src");
        // Remove small icons (flags, edit icons).
        double w = std::atof(attr(img, "width").c_str());
        if (w > 0 && w < 24) dom.remove(img);
    }
    for (Node block : querySelectorAll(root, ".thumb, figure[typeof], figure[class*=\"mw-\"], li.gallerybox, .tsingle")) {
        if (!isConnected(block)) continue;
        auto imgs = querySelectorAll(block, "img");
        if (imgs.empty()) {
            dom.remove(block);
            continue;
        }
        Node capSrc = querySelector(block, "figcaption, .thumbcaption, .gallerytext, .thumbcaption-inner");
        Node fig = dom.createElement("figure");
        for (Node i : imgs) {
            Node ni = dom.createElement("img");
            setAttr(ni, "src", attr(i, "src"));
            setAttr(ni, "alt", attr(i, "alt"));
            appendChild(fig, ni);
        }
        if (capSrc) {
            Node cap = dom.createElement("figcaption");
            for (Node c = capSrc->children; c; c = c->next) appendChild(cap, dom.copyNode(c));
            for (Node e : querySelectorAll(cap, ".magnify, sup.reference, .reference"))
                if (contains(cap, e)) dom.remove(e);
            if (!normText(textContent(cap)).empty()) {
                for (Node e : querySelectorAll(cap, "div, p")) dom.unwrap(e);
                appendChild(fig, cap);
            } else {
                dom.remove(cap);
            }
        }
        if (isTag(block, "li")) {
            // A gallery is a list. Move the figure out of the list.
            Node gallery = closest(block, "ul, ol");
            if (gallery) {
                insertBefore(gallery->parent, fig, gallery);
                dom.remove(block);
                if (!querySelector(gallery, "li")) dom.remove(gallery);
                continue;
            }
        }
        dom.replaceWith(block, fig);
    }
    // Math: keep the TeX text from the fallback image.
    for (Node m : querySelectorAll(root, ".mwe-math-element")) {
        if (!isConnected(m)) continue;
        std::string alt;
        if (Node img = querySelector(m, "img")) alt = attr(img, "alt");
        else if (Node ann = querySelector(m, "annotation")) alt = textContent(ann);
        Node code = dom.createElement("code");
        appendChild(code, dom.createText(normText(alt)));
        dom.replaceWith(m, code);
    }
}

} // namespace

Article extractWikipedia(const std::string& html, const std::string& url, const ExtractOptions& opts) {
    std::string base = url.empty() ? "https://en.wikipedia.org/wiki/" : url;
    auto dom = parseDocument(html);
    Node docRoot = dom->root();

    std::string title;
    if (Node fh = querySelector(docRoot, "#firstHeading")) title = normText(textContent(fh));
    if (title.empty()) {
        if (Node t = querySelector(docRoot, "title")) {
            static const std::regex suffix2(R"(\s+(-|\xE2\x80\x93|\xE2\x80\x94)\s+Wikipedia.*$)", std::regex::icase);
            title = std::regex_replace(normText(textContent(t)), suffix2, "");
        }
    }
    if (title.empty())
        if (Node h1 = querySelector(docRoot, "h1")) title = normText(textContent(h1));
    if (title.empty()) title = "Untitled";

    std::string htmlLang = attr(docRoot, "lang");
    if (htmlLang.empty())
        if (Node ct = querySelector(docRoot, "#mw-content-text")) htmlLang = attr(ct, "lang");
    std::string hostLang;
    {
        std::string host = hostOf(base);
        std::string sub = host.substr(0, host.find('.'));
        static const std::regex langRe(R"(^[a-z]{2,3}(-[a-z]+)?$)");
        if (std::regex_match(sub, langRe) && sub != "www") hostLang = sub;
    }

    Node root = querySelector(docRoot, "#mw-content-text .mw-parser-output");
    if (!root) root = querySelector(docRoot, "#mw-content-text");
    if (!root) root = querySelector(docRoot, ".mw-parser-output");
    if (!root) root = dom->body();

    std::string remove = kAlwaysRemove;
    if (!opts.keepInfobox) remove += ", .infobox, .infobox_v2, .infobox_v3";
    if (!opts.keepReferences) remove += std::string(", ") + kReferenceSelectors;
    for (Node el : querySelectorAll(root, remove))
        if (isConnected(el)) dom->remove(el);

    std::set<std::string> titles(kOtherSections.begin(), kOtherSections.end());
    if (!opts.keepReferences) titles.insert(kRefSections.begin(), kRefSections.end());
    else titles.insert({"sources", "works cited"});
    removeSections(*dom, root, titles);

    // Remove leftover "[edit]" text.
    for (Node h : querySelectorAll(root, "h1, h2, h3, h4, h5, h6")) {
        for (Node t : childNodes(h)) {
            if (t->type != XML_TEXT_NODE || !t->content) continue;
            std::string s = reinterpret_cast<const char*>(t->content);
            std::string r = removeEditMarks(s);
            if (r != s) dom->replaceWith(t, dom->createText(r));
        }
    }
    convertFigures(*dom, root, base);

    std::string firstPara;
    for (Node p : querySelectorAll(root, "p")) {
        std::string t = normText(textContent(p));
        if (u8len(t) > 40) {
            firstPara = t;
            break;
        }
    }
    std::string bodyHtml = sanitizeHtml(innerHtml(root), opts, base);

    ArticleParts parts;
    parts.bodyHtml = bodyHtml;
    parts.inputKind = InputKind::Wikipedia;
    parts.title = title;
    parts.author = "Wikipedia contributors";
    parts.siteName = "Wikipedia";
    parts.sourceUrl = url;
    parts.language = !htmlLang.empty() ? htmlLang : !hostLang.empty() ? hostLang : "en";
    if (!firstPara.empty()) parts.excerpt = u8len(firstPara) > 280 ? rtrimSpace(u8prefix(firstPara, 277)) + "..." : firstPara;
    return finishArticle(parts);
}

} // namespace ks::extract
