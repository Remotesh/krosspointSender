// Internal helpers for the output module. Do not include this header
// outside src/output. The public API is in output.h.
#pragma once

#include "output/output.h"

#include <libxml/tree.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ks::output::detail {

inline constexpr const char* kAppName = "k-rosspointSender";
inline constexpr const char* kXhtmlNs = "http://www.w3.org/1999/xhtml";
inline constexpr const char* kEpubNs = "http://www.idpf.org/2007/ops";
inline constexpr const char* kEpubMime = "application/epub+zip";

// ----------------------------------------------------------------- text

std::u32string toU32(const std::string& s);     // invalid bytes are skipped
std::string toU8(const std::u32string& s);
std::string toU8(char32_t c);
bool isJsSpace(char32_t c);                     // same set as the JS \s class
size_t cpLength(const std::string& s);          // number of code points
std::string cleanXmlText(const std::string& s); // removes chars that XML 1.0 does not allow
std::string escapeXml(const std::string& s);    // cleanXmlText + ks::xmlEscape
std::string collapseWs(const std::string& s);   // clean, collapse whitespace runs, trim
std::string jsTrim(const std::string& s);       // trim JS whitespace
std::string isoDay(const std::string& iso);     // "YYYY-MM-DD" or ""
std::string langOf(const Article& a);           // valid language tag or "en"
std::string pad(int n, int width);
std::string bookIdentifier(const std::string& id);
std::string sourceNoteText(const Article& a);
int countTextWords(const std::string& text);
std::string repeatStr(const std::string& s, size_t n);
std::vector<std::string> splitLines(const std::string& s);

struct Line {
    std::string label;
    std::string value;
};
std::vector<Line> coverLines(const Article& a);

// ----------------------------------------------------------------- DOM

// Owns one libxml2 document.
class Doc {
public:
    Doc() = default;
    explicit Doc(xmlDocPtr d) : p_(d) {}
    ~Doc() { reset(); }
    Doc(const Doc&) = delete;
    Doc& operator=(const Doc&) = delete;
    Doc(Doc&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Doc& operator=(Doc&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    xmlDocPtr get() const { return p_; }
    void reset() {
        if (p_) xmlFreeDoc(p_);
        p_ = nullptr;
    }

private:
    xmlDocPtr p_ = nullptr;
};

std::string nameOf(xmlNodePtr n);                          // lower-case local name ("" for non-elements)
bool isElement(xmlNodePtr n);
bool isText(xmlNodePtr n);                                 // text or CDATA
std::string textOf(xmlNodePtr n);                          // textContent
std::string attrOf(xmlNodePtr n, const char* name);        // "" when missing
bool hasAttr(xmlNodePtr n, const char* name);
std::vector<xmlNodePtr> childNodes(xmlNodePtr n);
std::vector<xmlNodePtr> childElements(xmlNodePtr n);
// Descendant elements (not `n` itself) in document order that match `pred`.
std::vector<xmlNodePtr> descendants(xmlNodePtr n, const std::function<bool(xmlNodePtr)>& pred);
bool isHeading(xmlNodePtr n);
int headingLevel(xmlNodePtr n);

// Parses an XHTML fragment. `root` receives the element that holds the
// fragment nodes. XML parser first, HTML parser as the fallback.
Doc parseFragment(const std::string& html, xmlNodePtr* root);

struct PreparedBody {
    Doc doc;
    xmlNodePtr root = nullptr;  // its children are the top-level blocks
};

// Same steps as the web prepareBody(): drop tags, tables, ids, links, wrappers.
PreparedBody prepareBody(const Article& a, const StyleSettings& style);

struct CopyContext {
    // Maps an <img> src to the output src. nullopt drops the image (alt text stays).
    std::function<std::optional<std::string>(const std::string& src, const std::string& alt)> image;
    // Maps a link href. nullopt keeps only the link text.
    std::function<std::optional<std::string>(const std::string& raw)> href;
};

// Creates an element in `doc` with the namespace `ns` (can be null).
xmlNodePtr newEl(xmlDocPtr doc, xmlNsPtr ns, const char* tag, const char* cls = nullptr);
xmlNodePtr addEl(xmlNodePtr parent, xmlNsPtr ns, const char* tag, const char* cls = nullptr);
void addText(xmlNodePtr parent, const std::string& text);

void copyNode(xmlNodePtr node, xmlNodePtr outParent, xmlNsPtr ns, const CopyContext& ctx);
void copyNodes(const std::vector<xmlNodePtr>& nodes, xmlNodePtr outParent, xmlNsPtr ns, const CopyContext& ctx);
std::optional<std::string> defaultHref(const std::string& raw);
void appendTitleBlock(xmlNodePtr parent, xmlNsPtr ns, const std::string& title, const std::vector<Line>& lines);
void appendSourceNote(xmlNodePtr parent, xmlNsPtr ns, const std::string& text);

// Renders the prepared article body into a new <div> container (no namespace).
struct Rendered {
    Doc doc;
    xmlNodePtr container = nullptr;
};
Rendered renderBody(const Article& a, const BuildOptions& o, const CopyContext* ctx = nullptr);
void renderBodyInto(const Article& a, const BuildOptions& o, xmlNodePtr parent, const CopyContext* ctx = nullptr);

// HTML5 serialization of a node (void tags without a slash).
void serializeHtml(xmlNodePtr n, std::string& out);
// XML serialization of an element through libxml2 (UTF-8, no declaration).
std::string serializeXml(xmlNodePtr n);

// ----------------------------------------------------------------- converters

std::string htmlToText(xmlNodePtr root, bool keepLinks, bool includeImages);
std::string htmlToMarkdown(xmlNodePtr root);

struct ZipEntry {
    std::string name;
    const std::string* data = nullptr;
    bool store = false;
};
// Writes a ZIP with miniz. Entries keep their order. No directory entries.
std::string zipPack(const std::vector<ZipEntry>& entries);

// EPUB bytes and the page estimate. buildZip uses this too.
std::string buildEpubBytes(const Article& a, const BuildOptions& o, const ImageMap& images, int* pages);

std::string articleJson(const Article& a);

} // namespace ks::output::detail
