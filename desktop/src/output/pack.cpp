// Spider pack EPUB builder (EPUB 3 + NCX fallback). Port of src/epub/pack.ts.
// Layout:
//   mimetype                              (first entry, stored)
//   META-INF/container.xml
//   EPUB/package.opf, EPUB/nav.xhtml, EPUB/toc.ncx, EPUB/css/krosspoint.css
//   EPUB/articles/index.xhtml             title page + "Included articles"
//   EPUB/articles/<slug>.xhtml            one file per bundled page
//   EPUB/articles/external-links.xhtml    external links, grouped by the holding article
//   EPUB/articles/missing.xhtml           linked pages that the pack does not hold
//   EPUB/articles/attribution.xhtml       sources, capture dates, license note
//   EPUB/images/<slug>_01.jpg ...
// The small nav/NCX helpers are copies of the ones in epub.cpp (they are
// private there). Every XHTML file is a libxml2 tree, so it is well-formed.
#include "output/internal.h"
#include "output/pack_parts.h"
#include "spider/spider.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>

namespace ks::output {

using namespace detail;
using namespace detail::packparts;

namespace {

const double kMB = 1024.0 * 1024.0;

bool isBundled(const PackPage& p) { return p.state == PackPageState::Done && p.article.has_value(); }

struct Resolved {
    std::vector<PackPage> pages;
    std::vector<PackExternalLink> external;
    std::map<std::string, std::string> renames;
};

bool safeBase(const std::string& b) {
    if (b.empty() || !(std::isalnum(static_cast<unsigned char>(b[0])) || b[0] == '_')) return false;
    for (unsigned char c : b)
        if (c >= 0x80 || !(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

// Bundled pages with a unique, safe fileName, and the external links.
Resolved resolvePages(const Pack& pack) {
    Resolved r;
    for (const auto& p : pack.pages)
        if (isBundled(p)) r.pages.push_back(p);
    std::vector<PackExternalLink> external = pack.externalLinks;
    bool needNames = std::any_of(r.pages.begin(), r.pages.end(), [](const PackPage& p) { return p.fileName.empty(); });
    if (needNames) {
        spider::Rewritten rw = spider::rewriteLinks(r.pages, pack.kind);
        r.pages.clear();
        for (auto& p : rw.pages)
            if (isBundled(p)) r.pages.push_back(std::move(p));
        external.insert(external.end(), rw.external.begin(), rw.external.end());
    }
    std::set<std::string> seen;
    for (const auto& e : external)
        if (seen.insert(e.fromKey + '\0' + e.key).second) r.external.push_back(e);

    std::set<std::string> used = {kIndexFile, kExternalFile, kMissingFile, kAttributionFile, "nav.xhtml"};
    for (auto& p : r.pages) {
        const std::string original = p.fileName;
        std::string base = original;
        if (base.size() > 6 && toLower(base.substr(base.size() - 6)) == ".xhtml") base = base.substr(0, base.size() - 6);
        if (!safeBase(base)) base = spider::packSlug(!p.candidate.title.empty() ? p.candidate.title : p.article->title);
        std::string name = base + ".xhtml";
        if (used.count(name)) {
            name = base + "-page.xhtml";
            for (int n = 2; used.count(name); ++n) name = base + "-page-" + std::to_string(n) + ".xhtml";
        }
        used.insert(name);
        if (!original.empty() && original != name && !r.renames.count(original)) r.renames[original] = name;
        p.fileName = name;
    }
    return r;
}

struct PackImage {
    std::string src, path, id, mime, bytes;
};

struct PreparedPage {
    const PackPage* page = nullptr;
    std::string title, fileName, slug;
    PreparedBody body;
    std::vector<xmlNodePtr> nodes;
    std::set<std::string> ids;
    std::vector<PackImage> images;
    size_t imageBytes = 0;
};

std::vector<PackImage> collectPageImages(const std::vector<xmlNodePtr>& nodes, const std::string& slug, int max, const BuildOptions& o,
                                         const ImageMap& images) {
    std::vector<PackImage> out;
    if (!o.includeImages || images.empty() || max <= 0) return out;
    std::vector<std::string> srcs;
    for (xmlNodePtr n : nodes) {
        if (!isElement(n)) continue;
        std::vector<xmlNodePtr> list = nameOf(n) == "img" ? std::vector<xmlNodePtr>{n} : descendants(n, [](xmlNodePtr e) { return nameOf(e) == "img"; });
        for (xmlNodePtr img : list) {
            std::string s = trim(attrOf(img, "src"));
            if (!s.empty() && std::find(srcs.begin(), srcs.end(), s) == srcs.end()) srcs.push_back(s);
        }
    }
    const StyleSettings& st = o.style;
    const int maxWidth = st.imageMaxWidthPx > 0 ? std::min(st.imageMaxWidthPx, 2000) : 480;
    for (const std::string& src : srcs) {
        if (static_cast<int>(out.size()) >= max) break;
        auto it = images.find(src);
        if (it == images.end() || it->second.empty()) continue;
        auto res = processImage(it->second, maxWidth, st.grayscaleImages, st.imageQuality);
        if (!res) continue;
        std::string base = slug + "_" + pad(static_cast<int>(out.size()) + 1, 2);
        std::string ext = res->mime == "image/png" ? "png" : "jpg";
        out.push_back({src, "images/" + base + "." + ext, xmlId("img", base), res->mime, std::move(res->bytes)});
    }
    return out;
}

CopyContext articleCopyContext(const PreparedPage& p, const std::map<std::string, std::string>& imageMap,
                               const std::map<std::string, std::set<std::string>>& fileIds,
                               const std::map<std::string, std::string>& renames) {
    const std::string base = !p.page->article->sourceUrl.empty() ? p.page->article->sourceUrl : p.page->candidate.url;
    CopyContext ctx;
    ctx.image = [&imageMap](const std::string& src, const std::string&) -> std::optional<std::string> {
        auto it = imageMap.find(src);
        if (it == imageMap.end()) return std::nullopt;
        return it->second;
    };
    ctx.href = [&p, &fileIds, &renames, base](const std::string& raw) -> std::optional<std::string> {
        if (raw.empty()) return std::nullopt;
        if (raw[0] == '#') {
            std::string id = raw.substr(1);
            if (!id.empty() && p.ids.count(id)) return raw;
            return std::nullopt;
        }
        // A link to another bundled page, set by rewriteLinks.
        std::string file = raw.substr(0, raw.find('#'));
        std::string frag = raw.find('#') == std::string::npos ? "" : raw.substr(raw.find('#') + 1);
        if (isPageFileName(file)) {
            auto rn = renames.find(file);
            if (rn != renames.end()) file = rn->second;
            auto ids = fileIds.find(file);
            if (ids == fileIds.end()) return std::nullopt;
            bool hasFrag = !frag.empty() && ids->second.count(frag);
            if (file == p.fileName) return hasFrag ? std::optional<std::string>("#" + frag) : std::optional<std::string>(file);
            return hasFrag ? file + "#" + frag : file;
        }
        if (auto keep = defaultHref(raw)) return keep;
        // Protocol-relative or site-relative links become absolute.
        return absoluteUrl(raw, base);
    };
    return ctx;
}

std::string articleXhtml(const PreparedPage& p, const std::string& lang, bool withImages,
                         const std::map<std::string, std::set<std::string>>& fileIds, const std::map<std::string, std::string>& renames) {
    Xhtml x = newXhtml(p.title, lang, kCssFromArticles);
    xmlNodePtr section = addEl(x.body, x.ns, "section");
    addTextEl(section, x.ns, "h1", p.title);
    const Article& a = *p.page->article;
    std::string site = collapseWs(a.siteName);
    if (site.empty()) site = hostOf(!a.sourceUrl.empty() ? a.sourceUrl : p.page->candidate.url);
    if (site.empty()) site = "the web";
    std::string day = isoDay(a.capturedAt);
    if (day.empty()) day = isoDay(nowIso());
    addTextEl(section, x.ns, "p", "Source: " + site + ", captured " + day, "source");
    std::map<std::string, std::string> imageMap;
    if (withImages)
        for (const auto& im : p.images) imageMap[im.src] = "../" + im.path;
    copyNodes(p.nodes, section, x.ns, articleCopyContext(p, imageMap, fileIds, renames));
    return serializeDoc(x);
}

std::string formatMB(double bytes) {
    double v = std::round(bytes / kMB * 100.0) / 100.0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

void report(const PackProgressFn& fn, PackPhase phase, int done, int total, const std::string& current, const std::string& message,
            const std::vector<PackError>& errors) {
    if (!fn) return;
    PackProgress p;
    p.phase = phase;
    p.done = done;
    p.total = total;
    p.currentTitle = current;
    p.message = message;
    p.errors = errors;
    try {
        fn(p);
    } catch (...) {
        // A failing listener must not stop the build.
    }
}

} // namespace

OutputFile buildPackEpub(const Pack& pack, const BuildOptions& o, const ImageMap& images, const PackProgressFn& onProgress) {
    const StyleSettings& style = o.style;
    std::string title = collapseWs(pack.title);
    if (title.empty()) title = "K-rosspoint pack";
    const std::string today = isoDay(nowIso());
    std::vector<PackError> errors;
    for (const auto& p : pack.pages)
        if (p.state == PackPageState::Failed) errors.push_back({p.candidate.key, p.candidate.title, p.error.empty() ? "fetch failed" : p.error});

    Resolved res = resolvePages(pack);
    const std::string lang = res.pages.empty() ? "en" : langOf(*res.pages[0].article);
    const int maxImages = std::max(0, pack.limits.maxImagesPerArticle);
    const int count = static_cast<int>(res.pages.size());

    // Pass 1: prepare each page and process its images.
    std::vector<std::unique_ptr<PreparedPage>> prepared;
    int estimatedPages = 0;
    for (int i = 0; i < count; ++i) {
        const PackPage& page = res.pages[i];
        const Article& article = *page.article;
        auto pp = std::make_unique<PreparedPage>();
        pp->page = &page;
        pp->title = collapseWs(!article.title.empty() ? article.title : page.candidate.title);
        if (pp->title.empty()) pp->title = "Article " + std::to_string(i + 1);
        report(onProgress, PackPhase::Building, i, count, pp->title, "", errors);
        pp->fileName = page.fileName;
        pp->slug = page.fileName.substr(0, page.fileName.size() - 6);
        pp->body = prepareBody(article, style);
        pp->nodes = childNodes(pp->body.root);
        for (const auto& id : idsIn(pp->nodes)) pp->ids.insert(id);
        pp->images = collectPageImages(pp->nodes, pp->slug, maxImages, o, images);
        for (const auto& im : pp->images) pp->imageBytes += im.bytes.size();
        estimatedPages += estimateReading(article, style).pages;
        prepared.push_back(std::move(pp));
    }

    std::map<std::string, std::set<std::string>> fileIds;
    for (const auto& p : prepared) fileIds[p->fileName] = p->ids;
    std::vector<PageRef> refs;
    for (const auto& p : prepared) refs.push_back({p->page, p->title, p->fileName});

    // The generated pages do not change between size passes.
    TextFile indexFile{kIndexFile, "index", indexXhtml(pack, refs, lang, title, today)};
    std::vector<TextFile> tail = {
        {kExternalFile, "external-links", externalXhtml(res.external, refs, lang)},
        {kMissingFile, "missing", missingXhtml(pack, res.external, refs, lang)},
        {kAttributionFile, "attribution", attributionXhtml(pack, refs, lang, today)},
    };

    // Navigation: index, each article with its headings, then the generated pages.
    std::vector<NavItem> navItems = {{0, "Title page", std::string("articles/") + kIndexFile}};
    for (const auto& p : prepared) {
        navItems.push_back({0, p->title, "articles/" + p->fileName});
        for (xmlNodePtr h : headingsIn(p->nodes))
            navItems.push_back({std::max(1, headingLevel(h)), headingLabel(h, "Section"), "articles/" + p->fileName + "#" + attrOf(h, "id")});
    }
    navItems.push_back({0, "External links", std::string("articles/") + kExternalFile});
    navItems.push_back({0, "Missing linked articles", std::string("articles/") + kMissingFile});
    navItems.push_back({0, "Sources and attribution", std::string("articles/") + kAttributionFile});

    BookMeta book;
    book.identifier = bookIdentifier(pack.id);
    book.title = title;
    book.creator = collapseWs(pack.author);
    if (book.creator.empty()) book.creator = pack.kind == PackSourceKind::Wikipedia ? "Wikipedia contributors" : "Various";
    book.language = lang;
    book.date = today;
    if (!pack.rootUrl.empty()) book.sources.push_back(pack.rootUrl);
    book.nav = buildTree(navItems);
    const std::string css = epubCss(style) + kPackCss;
    const std::string nav = navXhtml(book.nav, lang, kCssPath);
    const std::string ncx = ncxXml(book);

    // Writes the ZIP. Pages in `noImages` get no images.
    auto assemble = [&](const std::set<const PreparedPage*>& noImages) {
        std::vector<TextFile> files = {indexFile};
        for (size_t i = 0; i < prepared.size(); ++i) {
            const PreparedPage& p = *prepared[i];
            files.push_back({p.fileName, "art-" + pad(static_cast<int>(i + 1), 3), articleXhtml(p, lang, !noImages.count(&p), fileIds, res.renames)});
        }
        files.insert(files.end(), tail.begin(), tail.end());
        std::vector<const PackImage*> imgs;
        std::vector<ManifestImage> manifestImages;
        for (const auto& p : prepared)
            if (!noImages.count(p.get()))
                for (const auto& im : p->images) {
                    imgs.push_back(&im);
                    manifestImages.push_back({im.id, im.path, im.mime});
                }
        const std::string mimetype = kEpubMime;
        const std::string container = kPackContainerXml;
        const std::string opf = packOpf(book, files, manifestImages);
        std::vector<ZipEntry> e;
        e.push_back({"mimetype", &mimetype, true});
        e.push_back({"META-INF/container.xml", &container, false});
        e.push_back({"EPUB/package.opf", &opf, false});
        e.push_back({"EPUB/nav.xhtml", &nav, false});
        e.push_back({"EPUB/toc.ncx", &ncx, false});
        e.push_back({std::string("EPUB/") + kCssPath, &css, false});
        for (const auto& f : files) e.push_back({"EPUB/articles/" + f.file, &f.xhtml, false});
        for (const PackImage* im : imgs) e.push_back({"EPUB/" + im->path, &im->bytes, false});
        return zipPack(e);
    };

    // Size limit: remove the images of the largest pages first.
    const int limitMB = pack.limits.maxTotalEpubSizeMB;
    const double limitBytes = limitMB > 0 ? limitMB * kMB : 1e300;
    std::vector<const PreparedPage*> byImageSize;
    for (const auto& p : prepared)
        if (!p->images.empty()) byImageSize.push_back(p.get());
    std::stable_sort(byImageSize.begin(), byImageSize.end(), [](const PreparedPage* a, const PreparedPage* b) { return a->imageBytes > b->imageBytes; });
    std::set<const PreparedPage*> noImages;
    std::string bytes = assemble(noImages);
    for (size_t next = 0; static_cast<double>(bytes.size()) > limitBytes && next < byImageSize.size(); ++next) {
        noImages.insert(byImageSize[next]);
        bytes = assemble(noImages);
    }
    if (static_cast<double>(bytes.size()) > limitBytes) {
        std::string message = "Pack is " + formatMB(static_cast<double>(bytes.size())) + " MB; the limit is " + std::to_string(limitMB) +
                              " MB. Lower the article count or the image limit.";
        report(onProgress, PackPhase::Failed, count, count, "", message, errors);
        throw std::runtime_error(message);
    }
    const size_t dropped = noImages.size();
    report(onProgress, PackPhase::Complete, count, count, "",
           dropped ? "Removed the images of " + std::to_string(dropped) + " article(s) to stay under the size limit." : "", errors);

    OutputFile f;
    f.name = sanitizeFilename(title) + ".epub";
    f.mime = kEpubMime;
    f.bytes = std::move(bytes);
    f.format = OutputFormat::Epub;
    f.estimatedPages = std::max(1, estimatedPages);
    return f;
}

} // namespace ks::output
