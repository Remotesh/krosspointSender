// Parts of the spider pack EPUB builder: XHTML documents, navigation, OPF and
// the generated pages (index, external links, missing, attribution).
// Port of the helpers in src/epub/pack.ts and src/epub/build.ts.
#include "output/pack_parts.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <set>

namespace ks::output::detail::packparts {

const char* const kPackCss =
    "\n/* Spider pack additions */\n"
    ".source { font-size: 0.85em; font-style: italic; margin: 0 0 1em 0; text-indent: 0; }\n"
    ".pack-list li { margin: 0.2em 0; }\n"
    ".pack-url { font-size: 0.8em; word-wrap: break-word; }\n";

const char* const kPackContainerXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"EPUB/package.opf\" media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n";

const char* const kCcBySaUrl = "https://creativecommons.org/licenses/by-sa/4.0/";

// ----------------------------------------------------------------- XHTML

Xhtml newXhtml(const std::string& title, const std::string& lang, const std::string& cssHref) {
    Xhtml x;
    x.doc = Doc(xmlNewDoc(BAD_CAST "1.0"));
    xmlDocPtr d = x.doc.get();
    x.html = xmlNewDocNode(d, nullptr, BAD_CAST "html", nullptr);
    xmlDocSetRootElement(d, x.html);
    x.ns = xmlNewNs(x.html, BAD_CAST kXhtmlNs, nullptr);
    xmlNewNs(x.html, BAD_CAST kEpubNs, BAD_CAST "epub");
    xmlSetNs(x.html, x.ns);
    xmlSetProp(x.html, BAD_CAST "lang", BAD_CAST lang.c_str());
    xmlNsPtr xmlns = xmlSearchNs(d, x.html, BAD_CAST "xml");
    xmlSetNsProp(x.html, xmlns, BAD_CAST "lang", BAD_CAST lang.c_str());
    xmlNodePtr head = addEl(x.html, x.ns, "head");
    xmlSetProp(addEl(head, x.ns, "meta"), BAD_CAST "charset", BAD_CAST "utf-8");
    std::string tt = collapseWs(title);
    addText(addEl(head, x.ns, "title"), tt.empty() ? "Untitled" : tt);
    xmlNodePtr link = addEl(head, x.ns, "link");
    xmlSetProp(link, BAD_CAST "rel", BAD_CAST "stylesheet");
    xmlSetProp(link, BAD_CAST "type", BAD_CAST "text/css");
    xmlSetProp(link, BAD_CAST "href", BAD_CAST cssHref.c_str());
    x.body = addEl(x.html, x.ns, "body");
    return x;
}

std::string serializeDoc(const Xhtml& x) {
    if (!x.body->children) addEl(x.body, x.ns, "p");
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n" + serializeXml(x.html) + "\n";
}

xmlNodePtr addTextEl(xmlNodePtr parent, xmlNsPtr ns, const char* tag, const std::string& value, const char* cls) {
    xmlNodePtr e = addEl(parent, ns, tag, cls);
    addText(e, collapseWs(value));
    return e;
}

xmlNodePtr addLink(xmlNodePtr parent, xmlNsPtr ns, const std::string& href, const std::string& label) {
    xmlNodePtr a = addEl(parent, ns, "a");
    xmlSetProp(a, BAD_CAST "href", BAD_CAST cleanXmlText(href).c_str());
    std::string l = collapseWs(label);
    addText(a, l.empty() ? href : l);
    return a;
}

// ----------------------------------------------------------------- URLs and names

std::string xmlId(const std::string& prefix, const std::string& slug) {
    std::string out = prefix + "-";
    bool run = false;
    for (unsigned char c : slug) {
        bool ok = c < 0x80 && (std::isalnum(c) || c == '_' || c == '.' || c == '-');
        if (ok) {
            out += static_cast<char>(c);
            run = false;
        } else if (!run) {
            out += '-';
            run = true;
        }
    }
    return out;
}

namespace {

struct SimpleUrl {
    std::string scheme, host, path;
};

std::optional<SimpleUrl> splitUrl(const std::string& url) {
    static const std::regex re(R"(^([A-Za-z][A-Za-z0-9+.-]*)://([^/?#]*)([^?#]*))");
    std::smatch m;
    if (!std::regex_search(url, m, re)) return std::nullopt;
    return SimpleUrl{toLower(m[1].str()), toLower(m[2].str()), m[3].str()};
}

} // namespace

std::string hostOf(const std::string& url) {
    auto u = splitUrl(url);
    return u ? u->host : "";
}

std::optional<std::string> absoluteUrl(const std::string& raw, const std::string& base) {
    if (raw.empty()) return std::nullopt;
    if (auto u = splitUrl(raw)) {
        if (u->scheme == "http" || u->scheme == "https") return raw;
        return std::nullopt;
    }
    if (raw.rfind("//", 0) == 0) return "https:" + raw;
    auto b = splitUrl(base);
    if (!b || (b->scheme != "http" && b->scheme != "https")) return std::nullopt;
    if (raw[0] == '/') return b->scheme + "://" + b->host + raw;
    return std::nullopt;
}

bool isPageUrl(const std::string& url, PackSourceKind kind) {
    auto u = splitUrl(url);
    if (!u || (u->scheme != "http" && u->scheme != "https")) return false;
    if (kind == PackSourceKind::Wikipedia) {
        static const std::regex wiki(R"((^|\.)wikipedia\.org$)");
        return std::regex_search(u->host, wiki) && u->path.rfind("/wiki/", 0) == 0;
    }
    static const std::regex file(R"(\.(jpe?g|png|gif|webp|svg|pdf|zip|mp3|mp4)$)", std::regex::icase);
    return !std::regex_search(u->path, file);
}

bool isPageFileName(const std::string& file) {
    static const std::regex re(R"(^[A-Za-z0-9_][A-Za-z0-9_.%-]*\.xhtml$)", std::regex::icase);
    return std::regex_match(file, re);
}

// ----------------------------------------------------------------- navigation

std::vector<NavNode> buildTree(const std::vector<NavItem>& items) {
    struct Flat {
        NavItem item;
        std::vector<size_t> kids;
    };
    std::vector<Flat> flat;
    std::vector<size_t> roots;
    std::vector<std::pair<int, size_t>> stack;
    for (const NavItem& it : items) {
        size_t idx = flat.size();
        flat.push_back({it, {}});
        while (!stack.empty() && stack.back().first >= it.level) stack.pop_back();
        if (stack.empty()) roots.push_back(idx);
        else flat[stack.back().second].kids.push_back(idx);
        stack.push_back({it.level, idx});
    }
    std::function<NavNode(size_t)> make = [&](size_t i) {
        NavNode n{flat[i].item.label, flat[i].item.href, {}};
        for (size_t k : flat[i].kids) n.children.push_back(make(k));
        return n;
    };
    std::vector<NavNode> out;
    for (size_t r : roots) out.push_back(make(r));
    return out;
}

static void addNavList(xmlNodePtr parent, xmlNsPtr ns, const std::vector<NavNode>& nodes) {
    xmlNodePtr ol = addEl(parent, ns, "ol");
    for (const NavNode& n : nodes) {
        xmlNodePtr li = addEl(ol, ns, "li");
        addLink(li, ns, n.href, n.label);
        if (!n.children.empty()) addNavList(li, ns, n.children);
    }
}

std::string navXhtml(const std::vector<NavNode>& nav, const std::string& lang, const std::string& cssHref) {
    Xhtml x = newXhtml("Contents", lang, cssHref);
    xmlNodePtr navEl = addEl(x.body, x.ns, "nav");
    xmlSetNsProp(navEl, xmlSearchNs(x.doc.get(), x.html, BAD_CAST "epub"), BAD_CAST "type", BAD_CAST "toc");
    xmlSetProp(navEl, BAD_CAST "id", BAD_CAST "toc");
    addTextEl(navEl, x.ns, "h1", "Contents");
    addNavList(navEl, x.ns, nav);
    return serializeDoc(x);
}

static int navDepth(const std::vector<NavNode>& nodes) {
    int d = 0;
    for (const NavNode& n : nodes) d = std::max(d, 1 + navDepth(n.children));
    return d;
}

std::string ncxXml(const BookMeta& book) {
    int order = 0;
    std::function<std::string(const std::vector<NavNode>&, const std::string&)> points = [&](const std::vector<NavNode>& nodes,
                                                                                               const std::string& indent) {
        std::string out;
        for (size_t i = 0; i < nodes.size(); ++i) {
            const NavNode& n = nodes[i];
            // Take this point's number before the children take theirs.
            const std::string my = std::to_string(++order);
            std::string kids = n.children.empty() ? "" : "\n" + points(n.children, indent + "  ");
            if (i) out += "\n";
            out += indent + "<navPoint id=\"np-" + my + "\" playOrder=\"" + my + "\">\n";
            out += indent + "  <navLabel><text>" + escapeXml(n.label) + "</text></navLabel>\n";
            out += indent + "  <content src=\"" + escapeXml(n.href) + "\"/>" + kids + "\n";
            out += indent + "</navPoint>";
        }
        return out;
    };
    const std::string map = points(book.nav, "    ");
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\" xml:lang=\"" + escapeXml(book.language) + "\">\n";
    s += "  <head>\n";
    s += "    <meta name=\"dtb:uid\" content=\"" + escapeXml(book.identifier) + "\"/>\n";
    s += "    <meta name=\"dtb:depth\" content=\"" + std::to_string(std::max(1, navDepth(book.nav))) + "\"/>\n";
    s += "    <meta name=\"dtb:totalPageCount\" content=\"0\"/>\n";
    s += "    <meta name=\"dtb:maxPageNumber\" content=\"0\"/>\n";
    s += "  </head>\n";
    s += "  <docTitle><text>" + escapeXml(book.title) + "</text></docTitle>\n";
    if (!book.creator.empty()) s += "  <docAuthor><text>" + escapeXml(book.creator) + "</text></docAuthor>\n";
    s += "  <navMap>\n" + map + "\n  </navMap>\n</ncx>\n";
    return s;
}

std::vector<xmlNodePtr> headingsIn(const std::vector<xmlNodePtr>& nodes, int maxLevel) {
    std::vector<xmlNodePtr> out;
    for (xmlNodePtr n : nodes) {
        if (!isElement(n)) continue;
        std::vector<xmlNodePtr> all;
        if (isHeading(n)) all.push_back(n);
        for (xmlNodePtr h : descendants(n, isHeading)) all.push_back(h);
        for (xmlNodePtr h : all)
            if (headingLevel(h) <= maxLevel && !attrOf(h, "id").empty()) out.push_back(h);
    }
    return out;
}

std::vector<std::string> idsIn(const std::vector<xmlNodePtr>& nodes) {
    std::vector<std::string> out;
    for (xmlNodePtr n : nodes) {
        if (!isElement(n)) continue;
        if (!attrOf(n, "id").empty()) out.push_back(attrOf(n, "id"));
        for (xmlNodePtr e : descendants(n, [](xmlNodePtr x) { return hasAttr(x, "id"); })) out.push_back(attrOf(e, "id"));
    }
    return out;
}

std::string headingLabel(xmlNodePtr h, const std::string& fallback) {
    std::string s = collapseWs(textOf(h));
    return s.empty() ? fallback : s;
}

// ----------------------------------------------------------------- OPF

std::string packOpf(const BookMeta& book, const std::vector<TextFile>& files, const std::vector<ManifestImage>& images) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" xml:lang=\"" +
         escapeXml(book.language) + "\">\n";
    s += "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n";
    s += "    <dc:identifier id=\"bookid\">" + escapeXml(book.identifier) + "</dc:identifier>\n";
    s += "    <dc:title>" + escapeXml(book.title) + "</dc:title>\n";
    if (!book.creator.empty()) s += "    <dc:creator id=\"creator\">" + escapeXml(book.creator) + "</dc:creator>\n";
    s += "    <dc:language>" + escapeXml(book.language) + "</dc:language>\n";
    s += "    <dc:date>" + escapeXml(book.date) + "</dc:date>\n";
    for (const auto& src : book.sources) s += "    <dc:source>" + escapeXml(src) + "</dc:source>\n";
    s += "    <dc:subject>K-rosspoint pack</dc:subject>\n";
    s += "    <meta property=\"dcterms:modified\">" + nowIso() + "</meta>\n";
    s += std::string("    <meta name=\"generator\" content=\"") + kAppName + "\"/>\n";
    s += "  </metadata>\n  <manifest>\n";
    s += "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n";
    s += "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
    s += std::string("    <item id=\"css\" href=\"") + kCssPath + "\" media-type=\"text/css\"/>\n";
    for (const auto& f : files)
        s += "    <item id=\"" + escapeXml(f.id) + "\" href=\"articles/" + escapeXml(f.file) + "\" media-type=\"application/xhtml+xml\"/>\n";
    for (const auto& im : images)
        s += "    <item id=\"" + escapeXml(im.id) + "\" href=\"" + escapeXml(im.path) + "\" media-type=\"" + escapeXml(im.mime) + "\"/>\n";
    s += "  </manifest>\n  <spine toc=\"ncx\">\n";
    for (const auto& f : files) s += "    <itemref idref=\"" + escapeXml(f.id) + "\"/>\n";
    s += "  </spine>\n  <guide>\n";
    s += std::string("    <reference type=\"title-page\" title=\"Title page\" href=\"articles/") + kIndexFile + "\"/>\n";
    s += "    <reference type=\"toc\" title=\"Contents\" href=\"nav.xhtml\"/>\n";
    for (const auto& f : files)
        if (f.file != kIndexFile) {
            s += "    <reference type=\"text\" title=\"Start\" href=\"articles/" + escapeXml(f.file) + "\"/>\n";
            break;
        }
    s += "  </guide>\n</package>\n";
    return s;
}

// ----------------------------------------------------------------- generated pages

std::string indexXhtml(const Pack& pack, const std::vector<PageRef>& pages, const std::string& lang, const std::string& title,
                       const std::string& today) {
    Xhtml x = newXhtml(title, lang, kCssFromArticles);
    xmlNodePtr div = addEl(x.body, x.ns, "div", "cover");
    appendTitleBlock(div, x.ns, title, {{"Source", pack.rootUrl}, {"Articles", std::to_string(pages.size())}, {"Created", today}});
    addTextEl(x.body, x.ns, "h2", "Included articles");
    if (pages.empty()) {
        addTextEl(x.body, x.ns, "p", "This pack has no articles.");
    } else {
        xmlNodePtr ol = addEl(x.body, x.ns, "ol", "pack-list");
        for (const auto& p : pages) addLink(addEl(ol, x.ns, "li"), x.ns, p.fileName, p.title);
    }
    return serializeDoc(x);
}

std::string externalXhtml(const std::vector<PackExternalLink>& external, const std::vector<PageRef>& pages, const std::string& lang) {
    Xhtml x = newXhtml("External links", lang, kCssFromArticles);
    addTextEl(x.body, x.ns, "h1", "External links");
    std::vector<std::string> order;
    std::map<std::string, std::vector<const PackExternalLink*>> groups;
    for (const auto& e : external) {
        if (!absoluteUrl(e.url, "")) continue;
        if (!groups.count(e.fromKey)) order.push_back(e.fromKey);
        groups[e.fromKey].push_back(&e);
    }
    if (order.empty()) {
        addTextEl(x.body, x.ns, "p", "The articles in this pack have no external links.");
        return serializeDoc(x);
    }
    addTextEl(x.body, x.ns, "p", "These links go to pages outside this book. They need a network connection.");
    for (const auto& fromKey : order) {
        auto holder = std::find_if(pages.begin(), pages.end(), [&](const PageRef& p) { return p.page->candidate.key == fromKey; });
        xmlNodePtr h = addEl(x.body, x.ns, "h2");
        if (holder != pages.end()) addLink(h, x.ns, holder->fileName, holder->title);
        else addText(h, collapseWs(fromKey).empty() ? "Other" : collapseWs(fromKey));
        xmlNodePtr ul = addEl(x.body, x.ns, "ul", "pack-list");
        for (const auto* e : groups[fromKey]) addLink(addEl(ul, x.ns, "li"), x.ns, e->url, e->title.empty() ? e->url : e->title);
    }
    return serializeDoc(x);
}

std::string missingXhtml(const Pack& pack, const std::vector<PackExternalLink>& external, const std::vector<PageRef>& pages,
                         const std::string& lang) {
    Xhtml x = newXhtml("Missing linked articles", lang, kCssFromArticles);
    addTextEl(x.body, x.ns, "h1", "Missing linked articles");
    std::set<std::string> bundledKeys;
    for (const auto& p : pages) bundledKeys.insert(p.page->candidate.key);
    struct Item {
        std::string title, url, note;
    };
    std::vector<std::string> order;
    std::map<std::string, Item> items;
    // Pages that the pack tried to fetch but does not hold.
    for (const auto& p : pack.pages) {
        bool bundled = p.state == PackPageState::Done && p.article;
        if (bundled || bundledKeys.count(p.candidate.key) || items.count(p.candidate.key)) continue;
        auto url = absoluteUrl(p.candidate.url, "");
        if (!url) continue;
        std::string note = p.state == PackPageState::Failed ? "fetch failed" : p.state == PackPageState::Skipped ? "skipped" : "";
        order.push_back(p.candidate.key);
        items[p.candidate.key] = {p.candidate.title.empty() ? *url : p.candidate.title, *url, note};
    }
    // Linked pages that the pack does not hold.
    for (const auto& e : external) {
        if (bundledKeys.count(e.key) || items.count(e.key)) continue;
        auto url = absoluteUrl(e.url, "");
        if (!url || !isPageUrl(*url, pack.kind)) continue;
        order.push_back(e.key);
        items[e.key] = {e.title.empty() ? *url : e.title, *url, ""};
    }
    if (order.empty()) {
        addTextEl(x.body, x.ns, "p", "All linked articles that the scan found are in this book.");
        return serializeDoc(x);
    }
    addTextEl(x.body, x.ns, "p", "The articles in this book link to these pages. This book does not hold them.");
    xmlNodePtr ul = addEl(x.body, x.ns, "ul", "pack-list");
    for (const auto& key : order) {
        const Item& it = items[key];
        xmlNodePtr li = addEl(ul, x.ns, "li");
        addLink(li, x.ns, it.url, it.title);
        if (!it.note.empty()) addText(li, " (" + it.note + ")");
        addEl(li, x.ns, "br");
        addTextEl(li, x.ns, "span", it.url, "pack-url");
    }
    return serializeDoc(x);
}

std::string attributionXhtml(const Pack& pack, const std::vector<PageRef>& pages, const std::string& lang, const std::string& today) {
    Xhtml x = newXhtml("Sources and attribution", lang, kCssFromArticles);
    addTextEl(x.body, x.ns, "h1", "Sources and attribution");
    if (pack.kind == PackSourceKind::Wikipedia) {
        xmlNodePtr p = addEl(x.body, x.ns, "p");
        addText(p, "The articles come from Wikipedia. Text is available under the ");
        addLink(p, x.ns, kCcBySaUrl, "Creative Commons Attribution-ShareAlike 4.0 License");
        addText(p, ". The authors are the Wikipedia contributors. See the history of each article at its URL.");
    } else {
        addTextEl(x.body, x.ns, "p", "The articles come from the pages below. The rights stay with their authors.");
    }
    xmlNodePtr ol = addEl(x.body, x.ns, "ol", "pack-list");
    for (const auto& pg : pages) {
        xmlNodePtr li = addEl(ol, x.ns, "li");
        const Article& a = *pg.page->article;
        auto url = absoluteUrl(!a.sourceUrl.empty() ? a.sourceUrl : pg.page->candidate.url, "");
        addText(li, pg.title);
        std::string day = isoDay(a.capturedAt);
        if (!day.empty()) addText(li, ", captured " + day);
        if (url) {
            addEl(li, x.ns, "br");
            addLink(li, x.ns, *url, *url);
        }
    }
    addTextEl(x.body, x.ns, "p", std::string("Generated by ") + kAppName + " on " + today + ".");
    return serializeDoc(x);
}

} // namespace ks::output::detail::packparts
