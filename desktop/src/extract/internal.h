// K-rosspointSender desktop: extract module, internal helpers.
// Not part of the public API. Only files in src/extract include this header.
#pragma once

#include "extract/extract.h"

#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ks::extract::detail {

using Node = xmlNode*;

// ------------------------------------------------------------- text helpers

bool isUnicodeSpace(const std::string& s, size_t i, size_t* len);   // ASCII space, NBSP, U+2000.. etc.
std::string normText(const std::string& s);       // collapse whitespace, trim
size_t u8len(const std::string& s);               // code points
std::string u8prefix(const std::string& s, size_t codePoints);
std::string rtrimSpace(const std::string& s);
std::vector<std::string> splitWords(const std::string& s);  // split on whitespace, drop empty
std::string percentDecode(const std::string& s, bool plusIsSpace = false);
std::string encodeUriComponent(const std::string& s);
std::string escapeHtml(const std::string& s);     // & < > " '
bool startsWithNoCase(const std::string& s, const std::string& prefix);
bool equalsNoCase(const std::string& a, const std::string& b);
std::string cleanXmlText(const std::string& s);   // valid UTF-8, no XML-invalid code points

// ------------------------------------------------------------- URL helpers

struct UrlParts {
    std::string scheme, userinfo, host, port, path, query, fragment;
    bool hasQuery = false, hasFragment = false;
    std::string toString() const;
};
std::optional<UrlParts> parseAbsUrl(const std::string& url);   // scheme://authority/path?query#frag
std::string resolveUrl(const std::string& value, const std::string& base);   // RFC 3986; returns value on failure
std::string hostOf(const std::string& url);

// ------------------------------------------------------------- DOM

// A parsed document. Removed nodes go to a graveyard. We free them with the
// document, so pointers in a node list stay valid while we work.
class Dom {
public:
    explicit Dom(xmlDoc* d) : doc_(d) {}
    ~Dom();
    Dom(const Dom&) = delete;
    Dom& operator=(const Dom&) = delete;

    xmlDoc* doc() const { return doc_; }
    Node root() const;          // <html>
    Node body() const;          // <body>, created when missing
    Node head() const;          // <head> or null

    void remove(Node n);        // unlink and keep until destruction
    void unwrap(Node el);       // replace the element with its children
    void replaceWith(Node old, Node repl);
    Node createElement(const std::string& tag);
    Node createText(const std::string& text);
    Node copyNode(Node n);      // deep copy into this document

private:
    xmlDoc* doc_;
    std::vector<Node> trash_;
};

std::unique_ptr<Dom> parseDocument(const std::string& html);   // full page, charset sniffed
std::unique_ptr<Dom> parseFragment(const std::string& html);   // fragment, content goes in <body>

// Link primitives that never merge text nodes.
void insertBefore(Node parent, Node n, Node ref);   // ref null: append
inline void appendChild(Node parent, Node n) { insertBefore(parent, n, nullptr); }

bool isElement(Node n);
bool isText(Node n);
std::string tagName(Node n);                 // lowercase, "" for non-elements
bool isTag(Node n, const char* tag);
bool isHeading(Node n);
int headingLevelOfTag(Node n);               // 1..6 or 0
bool hasAttr(Node n, const char* name);
std::string attr(Node n, const char* name);  // "" when missing
void setAttr(Node n, const char* name, const std::string& value);
void removeAttr(Node n, const char* name);
void keepOnlyAttrs(Node n, const std::set<std::string>& keep);
std::vector<std::string> classList(Node n);
bool hasClass(Node n, const std::string& cls);
std::string textContent(Node n);             // all descendant text
void setTextContent(Dom& dom, Node n, const std::string& text);
bool isConnected(Node n);                    // still under the document
bool contains(Node ancestor, Node n);        // n inside ancestor (or equal)
Node parentElement(Node n);
Node firstElementChild(Node n);
std::vector<Node> childNodes(Node n);
std::vector<Node> elementChildren(Node n);
std::vector<Node> descendants(Node root);    // elements in document order, root excluded
std::string outerHtml(Node n);               // HTML serialization of the node
std::string innerHtml(Node n);               // HTML serialization of the children
std::string serializeXhtml(Node parent);     // XML serialization of the children

// A small CSS selector engine: tag, #id, .class, [attr], [attr="v"],
// [attr*="v"], [attr^="v"], [attr="v" i], the descendant combinator and
// selector lists ("a, b").
class Selector {
public:
    explicit Selector(const std::string& text);
    bool matches(Node n) const;
private:
    struct AttrTest { std::string name, op, value; bool icase = false; };
    struct Compound { std::string tag; std::vector<std::string> ids, classes; std::vector<AttrTest> attrs; };
    using Complex = std::vector<Compound>;   // left to right, descendant combinator
    static bool matchCompound(const Compound& c, Node n);
    bool matchComplex(const Complex& c, Node n) const;
    std::vector<Complex> list_;
};

std::vector<Node> querySelectorAll(Node root, const Selector& sel);
std::vector<Node> querySelectorAll(Node root, const std::string& sel);
Node querySelector(Node root, const Selector& sel);
Node querySelector(Node root, const std::string& sel);
Node closest(Node n, const std::string& sel);

// ------------------------------------------------------------- cleaning steps (sanitize.cpp)

bool isJunkToken(const std::string& token);
void removeJunk(Dom& dom, Node root, bool patterns);
void fixImages(Dom& dom, Node root, bool preferSrcset = false);
void normalizeFigures(Dom& dom, Node root);
void handleTables(Dom& dom, Node root, bool keepTables);
void linearize(Dom& dom, Node root);
void wrapInlineRuns(Dom& dom, Node parent);
bool isBlock(Node n);

// ------------------------------------------------------------- metadata (meta.cpp)

struct JsonLd { std::string headline, author, datePublished, publisher, description, inLanguage; };
struct PageMeta { std::string title, author, publishedAt, siteName, excerpt, language; std::vector<std::string> tags; };
std::string toIsoDate(const std::string& s);      // "" when not a date
JsonLd readJsonLd(Dom& dom);
PageMeta readMeta(Dom& dom);

// ------------------------------------------------------------- readability (readability.cpp)

struct ReaderResult {
    bool ok = false;
    std::string contentHtml;   // HTML of the chosen nodes
    std::string textContent;
    std::string title;
    std::string byline;
};
double textSimilarity(const std::string& a, const std::string& b);
ReaderResult readability(Dom& dom, const std::string& titleHint);

// ------------------------------------------------------------- finish (article.cpp)

struct ArticleParts {
    std::string bodyHtml;
    InputKind inputKind = InputKind::Html;
    std::string title, author, publishedAt, capturedAt, sourceUrl, siteName, excerpt, language;
    std::vector<std::string> tags;
};
Article finishArticle(const ArticleParts& p);
int countWordsNode(Node root);

std::string sanitizeHtmlImpl(const std::string& html, const ExtractOptions& opts, const std::string& baseUrl);

} // namespace ks::extract::detail
