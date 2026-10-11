// EPUB 3 builder with EPUB 2 fallbacks (NCX, guide). Port of epub/build.ts.
// Layout:
//   mimetype                    (first entry, stored)
//   META-INF/container.xml
//   OEBPS/content.opf
//   OEBPS/nav.xhtml             (EPUB 3 nav, epub:type="toc")
//   OEBPS/toc.ncx               (EPUB 2 NCX)
//   OEBPS/style.css
//   OEBPS/cover.xhtml           (text title page, when includeCover)
//   OEBPS/chapter-001.xhtml ...
//   OEBPS/images/img-001.jpg ...
// We build every XHTML file as a libxml2 tree and serialize it with
// libxml2. So each file is well-formed XML.
#include "output/internal.h"

#include <miniz.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace ks::output {

using namespace detail;

namespace {

// ----------------------------------------------------------------- types

struct NavNode {
    std::string label;
    std::string href;
    std::vector<NavNode> children;
};

struct NavItem {
    int level = 0;
    std::string label;
    std::string href;
};

struct ImageFile {
    std::string path;  // inside OEBPS, e.g. "images/img-001.jpg"
    std::string id;
    std::string mime;
    std::string bytes;
};

struct ChapterFile {
    std::string file;
    std::string id;
    std::string xhtml;
};

struct Book {
    std::string identifier;
    std::string title;
    std::string creator;
    std::string language;
    std::string date;
    std::vector<std::string> sources;
    std::vector<std::string> subjects;
    std::string description;
    std::string publisher;
    std::string css;
    std::string cover;  // empty: no cover
    std::vector<ChapterFile> chapters;
    std::vector<NavNode> nav;
    std::vector<ImageFile> images;
};

// ----------------------------------------------------------------- XHTML documents

struct Xhtml {
    Doc doc;
    xmlNodePtr html = nullptr;
    xmlNodePtr body = nullptr;
    xmlNsPtr ns = nullptr;
    xmlNsPtr epub = nullptr;
};

// Creates an XHTML document with head, title and the stylesheet link.
Xhtml newXhtml(const std::string& title, const std::string& lang) {
    Xhtml x;
    x.doc = Doc(xmlNewDoc(BAD_CAST "1.0"));
    xmlDocPtr d = x.doc.get();
    x.html = xmlNewDocNode(d, nullptr, BAD_CAST "html", nullptr);
    xmlDocSetRootElement(d, x.html);
    x.ns = xmlNewNs(x.html, BAD_CAST kXhtmlNs, nullptr);
    x.epub = xmlNewNs(x.html, BAD_CAST kEpubNs, BAD_CAST "epub");
    xmlSetNs(x.html, x.ns);
    xmlSetProp(x.html, BAD_CAST "lang", BAD_CAST lang.c_str());
    xmlNsPtr xmlns = xmlSearchNs(d, x.html, BAD_CAST "xml");
    xmlSetNsProp(x.html, xmlns, BAD_CAST "lang", BAD_CAST lang.c_str());
    xmlNodePtr head = addEl(x.html, x.ns, "head");
    xmlNodePtr meta = addEl(head, x.ns, "meta");
    xmlSetProp(meta, BAD_CAST "charset", BAD_CAST "utf-8");
    xmlNodePtr t = addEl(head, x.ns, "title");
    std::string tt = collapseWs(title);
    addText(t, tt.empty() ? "Untitled" : tt);
    xmlNodePtr link = addEl(head, x.ns, "link");
    xmlSetProp(link, BAD_CAST "rel", BAD_CAST "stylesheet");
    xmlSetProp(link, BAD_CAST "type", BAD_CAST "text/css");
    xmlSetProp(link, BAD_CAST "href", BAD_CAST "style.css");
    x.body = addEl(x.html, x.ns, "body");
    return x;
}

std::string serializeXhtml(const Xhtml& x) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n" + serializeXml(x.html) + "\n";
}

// ----------------------------------------------------------------- images

struct Collected {
    std::map<std::string, std::string> map;  // src -> local path
    std::vector<ImageFile> files;
};

// Processes the images of one article. Images without bytes or with a
// failed decode are not in the map, so the copy step drops them.
Collected collectImages(xmlNodePtr root, const BuildOptions& o, const ImageMap& images,
                        const std::function<std::string(int)>& baseName) {
    Collected c;
    if (!o.includeImages || images.empty() || !root) return c;
    std::vector<std::string> srcs;
    for (xmlNodePtr img : descendants(root, [](xmlNodePtr e) { return nameOf(e) == "img"; })) {
        std::string s = trim(attrOf(img, "src"));
        if (!s.empty() && std::find(srcs.begin(), srcs.end(), s) == srcs.end()) srcs.push_back(s);
    }
    const StyleSettings& st = o.style;
    const int maxWidth = st.imageMaxWidthPx > 0 ? std::min(st.imageMaxWidthPx, 2000) : 480;
    int n = 0;
    for (const std::string& src : srcs) {
        auto it = images.find(src);
        if (it == images.end() || it->second.empty()) continue;
        auto out = processImage(it->second, maxWidth, st.grayscaleImages, st.imageQuality);
        if (!out) continue;
        ++n;
        std::string base = baseName(n);
        std::string path = "images/" + base + (out->mime == "image/png" ? ".png" : ".jpg");
        c.map[src] = path;
        c.files.push_back({path, base, out->mime, std::move(out->bytes)});
    }
    return c;
}

// ----------------------------------------------------------------- chapters

struct Chunk {
    xmlNodePtr heading = nullptr;
    std::vector<xmlNodePtr> nodes;
};

bool hasContent(const std::vector<xmlNodePtr>& nodes) {
    for (xmlNodePtr n : nodes) {
        if (isText(n)) {
            if (!trim(textOf(n)).empty()) return true;
            continue;
        }
        if (!isElement(n)) continue;
        std::string name = nameOf(n);
        if (!trim(textOf(n)).empty() || name == "img" || name == "hr" || name == "table") return true;
        if (!descendants(n, [](xmlNodePtr e) {
                 std::string t = nameOf(e);
                 return t == "img" || t == "hr" || t == "table";
             }).empty())
            return true;
    }
    return false;
}

// Splits at each top-level h1/h2 when there is more than one of them.
std::vector<Chunk> splitChunks(xmlNodePtr root) {
    std::vector<xmlNodePtr> nodes = childNodes(root);
    auto isSplit = [](xmlNodePtr n) { int l = headingLevel(n); return l == 1 || l == 2; };
    size_t count = static_cast<size_t>(std::count_if(nodes.begin(), nodes.end(), isSplit));
    if (count <= 1) return {Chunk{nullptr, nodes}};
    std::vector<Chunk> chunks;
    Chunk cur;
    for (xmlNodePtr n : nodes) {
        if (isSplit(n)) {
            if (cur.heading || hasContent(cur.nodes)) chunks.push_back(cur);
            cur = Chunk{n, {n}};
        } else {
            cur.nodes.push_back(n);
        }
    }
    if (cur.heading || hasContent(cur.nodes)) chunks.push_back(cur);
    return chunks;
}

std::vector<xmlNodePtr> headingsIn(const std::vector<xmlNodePtr>& nodes, int maxLevel = 4) {
    std::vector<xmlNodePtr> all;
    for (xmlNodePtr n : nodes) {
        if (!isElement(n)) continue;
        if (isHeading(n)) all.push_back(n);
        for (xmlNodePtr h : descendants(n, isHeading)) all.push_back(h);
    }
    std::vector<xmlNodePtr> out;
    for (xmlNodePtr h : all)
        if (headingLevel(h) <= maxLevel && !attrOf(h, "id").empty()) out.push_back(h);
    return out;
}

std::vector<std::string> idsIn(const std::vector<xmlNodePtr>& nodes) {
    std::vector<std::string> out;
    for (xmlNodePtr n : nodes) {
        if (!isElement(n)) continue;
        std::string id = attrOf(n, "id");
        if (!id.empty()) out.push_back(id);
        for (xmlNodePtr e : descendants(n, [](xmlNodePtr x) { return hasAttr(x, "id"); }))
            out.push_back(attrOf(e, "id"));
    }
    return out;
}

std::string headingLabel(xmlNodePtr h, const std::string& fallback) {
    std::string s = collapseWs(textOf(h));
    return s.empty() ? fallback : s;
}

std::vector<NavNode> buildTree(const std::vector<NavItem>& items) {
    // Build with indices first. Pointers into growing vectors are not stable.
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

CopyContext makeCopyContext(const std::map<std::string, std::string>& imageMap,
                            const std::map<std::string, std::string>& idToFile, const std::string& currentFile,
                            bool includeImages) {
    CopyContext ctx;
    ctx.image = [&imageMap, includeImages](const std::string& src, const std::string&) -> std::optional<std::string> {
        if (!includeImages) return std::nullopt;
        auto it = imageMap.find(src);
        if (it == imageMap.end()) return std::nullopt;
        return it->second;
    };
    ctx.href = [&idToFile, currentFile](const std::string& raw) -> std::optional<std::string> {
        auto h = defaultHref(raw);
        if (!h) return std::nullopt;
        if ((*h)[0] == '#') {
            auto it = idToFile.find(h->substr(1));
            if (it == idToFile.end()) return std::nullopt;
            return it->second == currentFile ? *h : it->second + *h;
        }
        return h;
    };
    return ctx;
}

std::string bylineText(const Article& a) {
    std::string out;
    for (const std::string& part : {a.author, a.siteName, isoDay(a.publishedAt)}) {
        std::string v = collapseWs(part);
        if (v.empty()) continue;
        if (!out.empty()) out += " \xC2\xB7 ";  // " · "
        out += v;
    }
    return out;
}

void appendByline(xmlNodePtr body, xmlNsPtr ns, const Article& a) {
    std::string text = bylineText(a);
    if (text.empty()) return;
    xmlNodePtr p = addEl(body, ns, "p", "byline");
    addText(p, text);
}

// Makes sure a body is not empty.
void ensureContent(xmlNodePtr body, xmlNsPtr ns) {
    if (!body->children) addEl(body, ns, "p");
}

// ----------------------------------------------------------------- cover, nav, OPF, NCX

std::string coverXhtml(const std::string& title, const std::string& lang,
                       const std::function<void(xmlNodePtr, xmlNsPtr)>& fill) {
    Xhtml x = newXhtml(title, lang);
    xmlNodePtr div = newEl(x.doc.get(), x.ns, "div", "cover");
    xmlSetNsProp(div, x.epub, BAD_CAST "type", BAD_CAST "titlepage");
    fill(div, x.ns);
    xmlAddChild(x.body, div);
    return serializeXhtml(x);
}

void addNavList(xmlNodePtr parent, xmlNsPtr ns, const std::vector<NavNode>& nodes) {
    xmlNodePtr ol = addEl(parent, ns, "ol");
    for (const NavNode& n : nodes) {
        xmlNodePtr li = addEl(ol, ns, "li");
        xmlNodePtr a = addEl(li, ns, "a");
        xmlSetProp(a, BAD_CAST "href", BAD_CAST n.href.c_str());
        addText(a, n.label);
        if (!n.children.empty()) addNavList(li, ns, n.children);
    }
}

std::string navXhtml(const std::vector<NavNode>& nav, const std::string& lang) {
    Xhtml x = newXhtml("Contents", lang);
    xmlNodePtr navEl = newEl(x.doc.get(), x.ns, "nav");
    xmlSetNsProp(navEl, x.epub, BAD_CAST "type", BAD_CAST "toc");
    xmlSetProp(navEl, BAD_CAST "id", BAD_CAST "toc");
    xmlNodePtr h = addEl(navEl, x.ns, "h1");
    addText(h, "Contents");
    addNavList(navEl, x.ns, nav);
    xmlAddChild(x.body, navEl);
    return serializeXhtml(x);
}

int navDepth(const std::vector<NavNode>& nodes) {
    int d = 0;
    for (const NavNode& n : nodes) d = std::max(d, 1 + navDepth(n.children));
    return d;
}

std::string ncxXml(const Book& book) {
    int order = 0;
    std::function<std::string(const std::vector<NavNode>&, const std::string&)> points =
        [&](const std::vector<NavNode>& nodes, const std::string& indent) {
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
    s += "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\" xml:lang=\"" +
         escapeXml(book.language) + "\">\n";
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

std::string opfXml(const Book& book) {
    std::vector<std::string> meta;
    meta.push_back("    <dc:identifier id=\"bookid\">" + escapeXml(book.identifier) + "</dc:identifier>");
    meta.push_back("    <dc:title>" + escapeXml(book.title) + "</dc:title>");
    if (!book.creator.empty()) meta.push_back("    <dc:creator id=\"creator\">" + escapeXml(book.creator) + "</dc:creator>");
    meta.push_back("    <dc:language>" + escapeXml(book.language) + "</dc:language>");
    meta.push_back("    <dc:date>" + escapeXml(book.date) + "</dc:date>");
    for (const auto& s : book.sources) meta.push_back("    <dc:source>" + escapeXml(s) + "</dc:source>");
    if (!book.publisher.empty()) meta.push_back("    <dc:publisher>" + escapeXml(book.publisher) + "</dc:publisher>");
    if (!book.description.empty())
        meta.push_back("    <dc:description>" + escapeXml(book.description) + "</dc:description>");
    for (const auto& t : book.subjects) meta.push_back("    <dc:subject>" + escapeXml(t) + "</dc:subject>");
    meta.push_back("    <meta property=\"dcterms:modified\">" + nowIso() + "</meta>");
    meta.push_back(std::string("    <meta name=\"generator\" content=\"") + kAppName + "\"/>");

    std::vector<std::string> items = {
        "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>",
        "    <item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>",
        "    <item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>",
    };
    if (!book.cover.empty())
        items.push_back("    <item id=\"cover\" href=\"cover.xhtml\" media-type=\"application/xhtml+xml\"/>");
    for (const auto& c : book.chapters)
        items.push_back("    <item id=\"" + escapeXml(c.id) + "\" href=\"" + escapeXml(c.file) +
                        "\" media-type=\"application/xhtml+xml\"/>");
    for (const auto& im : book.images)
        items.push_back("    <item id=\"" + escapeXml(im.id) + "\" href=\"" + escapeXml(im.path) + "\" media-type=\"" +
                        escapeXml(im.mime) + "\"/>");
    std::vector<std::string> spine;
    if (!book.cover.empty()) spine.push_back("    <itemref idref=\"cover\"/>");
    for (const auto& c : book.chapters) spine.push_back("    <itemref idref=\"" + escapeXml(c.id) + "\"/>");
    std::vector<std::string> guide;
    if (!book.cover.empty()) guide.push_back("    <reference type=\"cover\" title=\"Cover\" href=\"cover.xhtml\"/>");
    guide.push_back("    <reference type=\"toc\" title=\"Contents\" href=\"nav.xhtml\"/>");
    if (!book.chapters.empty())
        guide.push_back("    <reference type=\"text\" title=\"Start\" href=\"" + escapeXml(book.chapters[0].file) + "\"/>");

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (size_t i = 0; i < v.size(); ++i) s += (i ? "\n" : "") + v[i];
        return s;
    };
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    s += "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\" xml:lang=\"" +
         escapeXml(book.language) + "\">\n";
    s += "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n";
    s += join(meta) + "\n  </metadata>\n";
    s += "  <manifest>\n" + join(items) + "\n  </manifest>\n";
    s += "  <spine toc=\"ncx\">\n" + join(spine) + "\n  </spine>\n";
    s += "  <guide>\n" + join(guide) + "\n  </guide>\n</package>\n";
    return s;
}

const char* kContainerXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n";

// Writes the ZIP. The "mimetype" entry is first and stored.
std::string packEpub(const Book& book) {
    const std::string mimetype = kEpubMime;
    const std::string container = kContainerXml;
    const std::string opf = opfXml(book);
    const std::string nav = navXhtml(book.nav, book.language);
    const std::string ncx = ncxXml(book);
    std::vector<ZipEntry> e;
    e.push_back({"mimetype", &mimetype, true});
    e.push_back({"META-INF/container.xml", &container, false});
    e.push_back({"OEBPS/content.opf", &opf, false});
    e.push_back({"OEBPS/nav.xhtml", &nav, false});
    e.push_back({"OEBPS/toc.ncx", &ncx, false});
    e.push_back({"OEBPS/style.css", &book.css, false});
    if (!book.cover.empty()) e.push_back({"OEBPS/cover.xhtml", &book.cover, false});
    for (const auto& c : book.chapters) e.push_back({"OEBPS/" + c.file, &c.xhtml, false});
    for (const auto& im : book.images) e.push_back({"OEBPS/" + im.path, &im.bytes, false});
    return zipPack(e);
}

std::string chapterFile(int n) { return "chapter-" + pad(n, 3) + ".xhtml"; }

OutputFile epubFile(const std::string& name, std::string bytes, int pages) {
    OutputFile f;
    f.name = name;
    f.mime = kEpubMime;
    f.bytes = std::move(bytes);
    f.format = OutputFormat::Epub;
    f.estimatedPages = pages;
    return f;
}

} // namespace

// ----------------------------------------------------------------- ZIP

std::string detail::zipPack(const std::vector<ZipEntry>& entries) {
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 64 * 1024)) throw std::runtime_error("zip: init failed");
    for (const ZipEntry& e : entries) {
        const mz_uint level = e.store ? static_cast<mz_uint>(MZ_NO_COMPRESSION) : static_cast<mz_uint>(MZ_DEFAULT_LEVEL);
        if (!mz_zip_writer_add_mem(&zip, e.name.c_str(), e.data->data(), e.data->size(), level)) {
            mz_zip_writer_end(&zip);
            throw std::runtime_error("zip: cannot add " + e.name);
        }
    }
    void* buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        mz_zip_writer_end(&zip);
        throw std::runtime_error("zip: finalize failed");
    }
    std::string out(static_cast<const char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return out;
}

// ----------------------------------------------------------------- public API

std::string detail::buildEpubBytes(const Article& article, const BuildOptions& o, const ImageMap& images, int* pages) {
    const StyleSettings& style = o.style;
    const std::string lang = langOf(article);
    std::string title = collapseWs(article.title);
    if (title.empty()) title = "Untitled";
    PreparedBody prepared = prepareBody(article, style);
    Collected imgs = collectImages(prepared.root, o, images, [](int n) { return "img-" + pad(n, 3); });

    std::vector<Chunk> chunks = prepared.root ? splitChunks(prepared.root) : std::vector<Chunk>{Chunk{}};
    const bool split = chunks.size() > 1 || chunks[0].heading != nullptr;
    std::vector<std::string> files;
    for (size_t i = 0; i < chunks.size(); ++i) files.push_back(chapterFile(static_cast<int>(i + 1)));
    std::map<std::string, std::string> idToFile;
    for (size_t i = 0; i < chunks.size(); ++i)
        for (const std::string& id : idsIn(chunks[i].nodes)) idToFile.emplace(id, files[i]);

    // Navigation items.
    std::vector<NavItem> navItems;
    int minLevel = 6;
    for (const Chunk& c : chunks)
        for (xmlNodePtr h : headingsIn(c.nodes)) minLevel = std::min(minLevel, headingLevel(h));
    for (size_t i = 0; i < chunks.size(); ++i) {
        const Chunk& c = chunks[i];
        // A preface chunk, or the only chunk.
        if (!c.heading) navItems.push_back({split ? minLevel : 0, title, files[i]});
        for (xmlNodePtr h : headingsIn(c.nodes)) {
            std::string id = attrOf(h, "id");
            std::string href = h == c.heading ? files[i] : files[i] + "#" + id;
            navItems.push_back({headingLevel(h), headingLabel(h, "Section"), href});
        }
    }

    std::vector<ChapterFile> chapters;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const Chunk& c = chunks[i];
        std::string chTitle = c.heading ? headingLabel(c.heading, title) : title;
        Xhtml x = newXhtml(chTitle, lang);
        if (i == 0 && !o.includeCover) {
            appendTitleBlock(x.body, x.ns, title, {});
            appendByline(x.body, x.ns, article);
        }
        CopyContext ctx = makeCopyContext(imgs.map, idToFile, files[i], o.includeImages);
        copyNodes(c.nodes, x.body, x.ns, ctx);
        if (i == chunks.size() - 1 && o.includeSourceNote) appendSourceNote(x.body, x.ns, sourceNoteText(article));
        ensureContent(x.body, x.ns);
        chapters.push_back({files[i], "chapter-" + pad(static_cast<int>(i + 1), 3), serializeXhtml(x)});
    }

    std::vector<NavNode> nav = buildTree(navItems);
    std::string cover;
    if (o.includeCover) {
        std::vector<Line> lines = coverLines(article);
        cover = coverXhtml(title, lang, [&](xmlNodePtr div, xmlNsPtr ns) { appendTitleBlock(div, ns, title, lines); });
        nav.insert(nav.begin(), NavNode{"Title page", "cover.xhtml", {}});
    }

    Book book;
    book.identifier = bookIdentifier(article.id);
    book.title = title;
    book.creator = collapseWs(article.author);
    book.language = lang;
    book.date = isoDay(article.publishedAt);
    if (book.date.empty()) book.date = isoDay(article.capturedAt);
    if (book.date.empty()) book.date = isoDay(nowIso());
    if (!article.sourceUrl.empty()) book.sources.push_back(article.sourceUrl);
    for (const auto& t : article.tags) {
        std::string v = collapseWs(t);
        if (!v.empty()) book.subjects.push_back(v);
    }
    book.description = collapseWs(article.excerpt);
    book.publisher = collapseWs(article.siteName);
    book.css = epubCss(style);
    book.cover = cover;
    book.chapters = std::move(chapters);
    book.nav = std::move(nav);
    book.images = std::move(imgs.files);
    if (pages) *pages = estimateReading(article, style).pages;
    return packEpub(book);
}

OutputFile buildEpub(const Article& a, const BuildOptions& o, const ImageMap& images) {
    int pages = 1;
    std::string bytes = buildEpubBytes(a, o, images, &pages);
    return epubFile(sanitizeFilename(a.title) + ".epub", std::move(bytes), pages);
}

OutputFile buildBundleEpub(const std::vector<Article>& articles, const std::string& metaTitle,
                           const std::string& metaAuthor, const BuildOptions& o, const ImageMap& images) {
    const StyleSettings& style = o.style;
    std::string bookTitle = collapseWs(metaTitle);
    if (bookTitle.empty()) bookTitle = "Articles";
    const std::string lang = articles.empty() ? "en" : langOf(articles[0]);
    std::vector<ChapterFile> chapters;
    std::vector<NavItem> navItems;
    std::vector<ImageFile> imageFiles;
    int pages = 0;

    for (size_t ai = 0; ai < articles.size(); ++ai) {
        const Article& article = articles[ai];
        const int num = static_cast<int>(ai + 1);
        const std::string file = chapterFile(num);
        std::string title = collapseWs(article.title);
        if (title.empty()) title = "Article " + std::to_string(num);
        PreparedBody prepared = prepareBody(article, style);
        Collected imgs = collectImages(prepared.root, o, images,
                                       [num](int n) { return "img-a" + pad(num, 2) + "-" + pad(n, 3); });
        for (auto& f : imgs.files) imageFiles.push_back(std::move(f));
        std::vector<xmlNodePtr> nodes = childNodes(prepared.root);
        std::map<std::string, std::string> idToFile;
        for (const std::string& id : idsIn(nodes)) idToFile[id] = file;

        navItems.push_back({0, title, file});
        for (xmlNodePtr h : headingsIn(nodes))
            navItems.push_back({headingLevel(h), headingLabel(h, "Section"), file + "#" + attrOf(h, "id")});

        Xhtml x = newXhtml(title, langOf(article));
        appendTitleBlock(x.body, x.ns, title, {});
        appendByline(x.body, x.ns, article);
        CopyContext ctx = makeCopyContext(imgs.map, idToFile, file, o.includeImages);
        copyNodes(nodes, x.body, x.ns, ctx);
        if (o.includeSourceNote) appendSourceNote(x.body, x.ns, sourceNoteText(article));
        chapters.push_back({file, "chapter-" + pad(num, 3), serializeXhtml(x)});
        pages += estimateReading(article, style).pages;
    }

    if (chapters.empty()) {
        Xhtml x = newXhtml(bookTitle, lang);
        appendTitleBlock(x.body, x.ns, bookTitle, {});
        xmlNodePtr p = addEl(x.body, x.ns, "p");
        addText(p, "This bundle has no articles.");
        chapters.push_back({chapterFile(1), "chapter-001", serializeXhtml(x)});
        navItems.push_back({0, bookTitle, chapterFile(1)});
        pages = 1;
    }

    std::vector<NavNode> nav = buildTree(navItems);
    const std::string author = collapseWs(metaAuthor);
    const std::string today = isoDay(nowIso());
    std::string cover;
    if (o.includeCover) {
        cover = coverXhtml(bookTitle, lang, [&](xmlNodePtr div, xmlNsPtr ns) {
            std::vector<Line> lines;
            if (!author.empty()) lines.push_back({"Author", author});
            lines.push_back({"Articles", std::to_string(articles.size())});
            lines.push_back({"Created", today});
            appendTitleBlock(div, ns, bookTitle, lines);
            if (!articles.empty()) {
                xmlNodePtr ol = addEl(div, ns, "ol");
                for (size_t i = 0; i < articles.size(); ++i) {
                    xmlNodePtr li = addEl(ol, ns, "li");
                    std::string t = collapseWs(articles[i].title);
                    addText(li, t.empty() ? "Article " + std::to_string(i + 1) : t);
                }
            }
        });
        nav.insert(nav.begin(), NavNode{"Title page", "cover.xhtml", {}});
    }

    Book book;
    book.identifier = "urn:uuid:" + newUuid();
    book.title = bookTitle;
    book.creator = author;
    book.language = lang;
    book.date = today;
    std::set<std::string> seenSrc, seenSub;
    for (const Article& a : articles)
        if (!a.sourceUrl.empty() && seenSrc.insert(a.sourceUrl).second) book.sources.push_back(a.sourceUrl);
    for (const Article& a : articles)
        for (const auto& t : a.tags) {
            std::string v = collapseWs(t);
            if (!v.empty() && seenSub.insert(v).second) book.subjects.push_back(v);
        }
    book.css = epubCss(style);
    book.cover = cover;
    book.chapters = std::move(chapters);
    book.nav = std::move(nav);
    book.images = std::move(imageFiles);
    return epubFile(sanitizeFilename(bookTitle) + ".epub", packEpub(book), std::max(1, pages));
}

OutputFile testEpub() {
    Article a;
    a.id = newUuid();
    a.title = "k-rosspointSender test";
    a.author = kAppName;
    a.capturedAt = nowIso();
    a.language = "en";
    a.bodyHtml =
        "<p>The transfer works. k-rosspointSender sent this book to your reader. "
        "You can delete this file now.</p>";
    a.wordCount = 16;
    a.inputKind = InputKind::Text;
    BuildOptions o;
    o.style = preset(StylePreset::Comfortable);
    o.includeImages = false;
    o.includeCover = false;
    o.includeSourceNote = false;
    std::string bytes = buildEpubBytes(a, o, {}, nullptr);
    return epubFile("krosspoint-test.epub", std::move(bytes), 1);
}

} // namespace ks::output
