// Internal parts of the spider pack EPUB builder (pack.cpp, pack_parts.cpp).
// Do not include this header outside src/output.
#pragma once

#include "output/internal.h"

#include <optional>
#include <string>
#include <vector>

namespace ks::output::detail::packparts {

inline constexpr const char* kIndexFile = "index.xhtml";
inline constexpr const char* kExternalFile = "external-links.xhtml";
inline constexpr const char* kMissingFile = "missing.xhtml";
inline constexpr const char* kAttributionFile = "attribution.xhtml";
inline constexpr const char* kCssPath = "css/krosspoint.css";
inline constexpr const char* kCssFromArticles = "../css/krosspoint.css";
extern const char* const kPackCss;
extern const char* const kPackContainerXml;

// ----------------------------------------------------------------- XHTML

struct Xhtml {
    Doc doc;
    xmlNodePtr html = nullptr;
    xmlNodePtr body = nullptr;
    xmlNsPtr ns = nullptr;
};
Xhtml newXhtml(const std::string& title, const std::string& lang, const std::string& cssHref);
std::string serializeDoc(const Xhtml& x);
// Adds <tag class=cls>value</tag> (whitespace collapsed).
xmlNodePtr addTextEl(xmlNodePtr parent, xmlNsPtr ns, const char* tag, const std::string& value, const char* cls = nullptr);
xmlNodePtr addLink(xmlNodePtr parent, xmlNsPtr ns, const std::string& href, const std::string& label);

// ----------------------------------------------------------------- URLs and names

std::string xmlId(const std::string& prefix, const std::string& slug);
std::string hostOf(const std::string& url);
std::optional<std::string> absoluteUrl(const std::string& raw, const std::string& base);   // http(s) only
bool isPageUrl(const std::string& url, PackSourceKind kind);
bool isPageFileName(const std::string& file);   // "steam_engine.xhtml"

// ----------------------------------------------------------------- navigation

struct NavItem {
    int level = 0;
    std::string label;
    std::string href;
};
struct NavNode {
    std::string label;
    std::string href;
    std::vector<NavNode> children;
};
std::vector<NavNode> buildTree(const std::vector<NavItem>& items);

struct BookMeta {
    std::string identifier, title, creator, language, date;
    std::vector<std::string> sources;
    std::vector<NavNode> nav;
};
std::string navXhtml(const std::vector<NavNode>& nav, const std::string& lang, const std::string& cssHref);
std::string ncxXml(const BookMeta& book);   // gap-free playOrder, parent before children

std::vector<xmlNodePtr> headingsIn(const std::vector<xmlNodePtr>& nodes, int maxLevel = 4);
std::vector<std::string> idsIn(const std::vector<xmlNodePtr>& nodes);
std::string headingLabel(xmlNodePtr h, const std::string& fallback);

// ----------------------------------------------------------------- OPF

struct TextFile {
    std::string file;   // inside EPUB/articles/
    std::string id;
    std::string xhtml;
};
struct ManifestImage {
    std::string id, path, mime;
};
std::string packOpf(const BookMeta& book, const std::vector<TextFile>& files, const std::vector<ManifestImage>& images);

// ----------------------------------------------------------------- generated pages

struct PageRef {
    const PackPage* page = nullptr;
    std::string title;
    std::string fileName;
};
std::string indexXhtml(const Pack& pack, const std::vector<PageRef>& pages, const std::string& lang, const std::string& title,
                       const std::string& today);
std::string externalXhtml(const std::vector<PackExternalLink>& external, const std::vector<PageRef>& pages, const std::string& lang);
std::string missingXhtml(const Pack& pack, const std::vector<PackExternalLink>& external, const std::vector<PageRef>& pages,
                         const std::string& lang);
std::string attributionXhtml(const Pack& pack, const std::vector<PageRef>& pages, const std::string& lang, const std::string& today);

} // namespace ks::output::detail::packparts
