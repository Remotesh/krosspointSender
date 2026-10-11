// k-rosspointSender desktop: extract module, DOM and text helpers on libxml2.
#include "extract/internal.h"

#include <libxml/HTMLtree.h>
#include <libxml/uri.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ks::extract::detail {

// ============================================================= text helpers

bool isUnicodeSpace(const std::string& s, size_t i, size_t* len) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') { *len = 1; return true; }
    if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) { *len = 2; return true; }
    if (i + 2 >= s.size()) return false;
    unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
    unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
    bool sp = false;
    if (c == 0xE1 && c1 == 0x9A && c2 == 0x80) sp = true;                       // U+1680
    else if (c == 0xE2 && c1 == 0x80 && (c2 <= 0x8A || c2 == 0xA8 || c2 == 0xA9 || c2 == 0xAF)) sp = true;  // U+2000..200A, 2028, 2029, 202F
    else if (c == 0xE2 && c1 == 0x81 && c2 == 0x9F) sp = true;                  // U+205F
    else if (c == 0xE3 && c1 == 0x80 && c2 == 0x80) sp = true;                  // U+3000
    else if (c == 0xEF && c1 == 0xBB && c2 == 0xBF) sp = true;                  // U+FEFF
    if (sp) *len = 3;
    return sp;
}

std::string normText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pendingSpace = false;
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        if (isUnicodeSpace(s, i, &len)) {
            pendingSpace = true;
            i += len;
            continue;
        }
        if (pendingSpace && !out.empty()) out += ' ';
        pendingSpace = false;
        out += s[i++];
    }
    return out;
}

size_t u8len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string u8prefix(const std::string& s, size_t codePoints) {
    size_t n = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (n == codePoints) return s.substr(0, i);
            ++n;
        }
    }
    return s;
}

std::string rtrimSpace(const std::string& s) {
    size_t end = 0;
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        if (isUnicodeSpace(s, i, &len)) { i += len; continue; }
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
        end = i;
    }
    return s.substr(0, end);
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size();) {
        size_t len = 0;
        if (isUnicodeSpace(s, i, &len)) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            i += len;
            continue;
        }
        cur += s[i++];
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& s, bool plusIsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexVal(s[i + 1]) >= 0 && hexVal(s[i + 2]) >= 0) {
            out += static_cast<char>(hexVal(s[i + 1]) * 16 + hexVal(s[i + 2]));
            i += 2;
        } else if (plusIsSpace && s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string encodeUriComponent(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || std::strchr("-_.!~*'()", c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string escapeHtml(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

bool startsWithNoCase(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

bool equalsNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && startsWithNoCase(a, b);
}

std::string cleanXmlText(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        unsigned cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        bool valid = len > 0 && i + len <= n;
        for (size_t k = 1; valid && k < len; ++k) {
            if ((p[i + k] & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (valid) {
            // Reject overlong forms, surrogates and out-of-range values.
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) valid = false;
            if (cp >= 0xD800 && cp <= 0xDFFF) valid = false;
        }
        if (!valid) {
            out += "\xEF\xBF\xBD";   // U+FFFD
            ++i;
            continue;
        }
        bool allowed = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp != 0xFFFE && cp != 0xFFFF);
        if (allowed) out.append(s, i, len);
        i += len;
    }
    return out;
}

// ============================================================= URL helpers

std::string UrlParts::toString() const {
    std::string out = scheme + "://";
    if (!userinfo.empty()) out += userinfo + "@";
    out += host;
    if (!port.empty()) out += ":" + port;
    out += path;
    if (hasQuery) out += "?" + query;
    if (hasFragment) out += "#" + fragment;
    return out;
}

std::optional<UrlParts> parseAbsUrl(const std::string& url) {
    size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return std::nullopt;
    UrlParts u;
    u.scheme = toLower(url.substr(0, sep));
    if (!std::isalpha(static_cast<unsigned char>(u.scheme[0]))) return std::nullopt;
    for (char c : u.scheme)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') return std::nullopt;
    std::string rest = url.substr(sep + 3);
    size_t aend = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, aend);
    std::string tail = aend == std::string::npos ? "" : rest.substr(aend);
    size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        u.userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }
    std::string hostPart = authority;
    if (!authority.empty() && authority[0] == '[') {
        size_t rb = authority.find(']');
        if (rb == std::string::npos) return std::nullopt;
        hostPart = authority.substr(0, rb + 1);
        std::string after = authority.substr(rb + 1);
        if (!after.empty()) {
            if (after[0] != ':') return std::nullopt;
            u.port = after.substr(1);
        }
    } else {
        size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            hostPart = authority.substr(0, colon);
            u.port = authority.substr(colon + 1);
        }
    }
    for (char c : u.port)
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    if (!u.port.empty()) {
        // Remove leading zeros, then the default port.
        size_t nz = u.port.find_first_not_of('0');
        u.port = nz == std::string::npos ? "0" : u.port.substr(nz);
        if (u.port.size() > 5 || std::stol(u.port) > 65535) return std::nullopt;
        if ((u.scheme == "http" && u.port == "80") || (u.scheme == "https" && u.port == "443")) u.port.clear();
    }
    u.host = toLower(hostPart);
    for (char c : u.host)
        if (std::strchr(" #%/<>?@\\^|\t\r\n", c)) return std::nullopt;
    size_t hashPos = tail.find('#');
    if (hashPos != std::string::npos) {
        u.hasFragment = true;
        u.fragment = tail.substr(hashPos + 1);
        tail = tail.substr(0, hashPos);
    }
    size_t q = tail.find('?');
    if (q != std::string::npos) {
        u.hasQuery = true;
        u.query = tail.substr(q + 1);
        tail = tail.substr(0, q);
    }
    u.path = tail;
    if (u.path.empty() && (u.scheme == "http" || u.scheme == "https")) u.path = "/";
    return u;
}

// Escape spaces and non-ASCII bytes so that the RFC 3986 parser accepts the URL.
static std::string preEscape(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || std::strchr("\"<>\\^`{|}", c)) {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string resolveUrl(const std::string& value, const std::string& base) {
    if (base.empty() || value.empty() || value[0] == '#') return value;
    std::string v = preEscape(value);
    std::string b = preEscape(base);
    if (v.rfind("//", 0) == 0) {
        // A protocol-relative URL takes the scheme of the base.
        auto bp = parseAbsUrl(base);
        v = (bp ? bp->scheme : std::string("https")) + ":" + v;
    }
    xmlChar* r = xmlBuildURI(BAD_CAST v.c_str(), BAD_CAST b.c_str());
    if (!r) return value;
    std::string out(reinterpret_cast<const char*>(r));
    xmlFree(r);
    // Lowercase the scheme and the host like the WHATWG URL parser.
    if (auto u = parseAbsUrl(out)) {
        if (u->scheme == "http" || u->scheme == "https") return u->toString();
    }
    return out;
}

std::string hostOf(const std::string& url) {
    auto u = parseAbsUrl(url);
    return u ? u->host : std::string();
}

// ============================================================= DOM

Dom::~Dom() {
    for (Node n : trash_) xmlFreeNode(n);
    if (doc_) xmlFreeDoc(doc_);
}

Node Dom::root() const { return xmlDocGetRootElement(doc_); }

Node Dom::body() const {
    Node html = root();
    if (!html) {
        html = xmlNewDocNode(doc_, nullptr, BAD_CAST "html", nullptr);
        xmlDocSetRootElement(doc_, html);
    }
    for (Node c = html->children; c; c = c->next)
        if (isTag(c, "body")) return c;
    Node b = xmlNewDocNode(doc_, nullptr, BAD_CAST "body", nullptr);
    appendChild(html, b);
    return b;
}

Node Dom::head() const {
    Node html = root();
    if (!html) return nullptr;
    for (Node c = html->children; c; c = c->next)
        if (isTag(c, "head")) return c;
    return nullptr;
}

void Dom::remove(Node n) {
    if (!n) return;
    xmlUnlinkNode(n);
    trash_.push_back(n);
}

void Dom::unwrap(Node el) {
    Node parent = el->parent;
    if (!parent) return;
    while (el->children) insertBefore(parent, el->children, el);
    remove(el);
}

void Dom::replaceWith(Node old, Node repl) {
    if (!old->parent) return;
    insertBefore(old->parent, repl, old);
    remove(old);
}

Node Dom::createElement(const std::string& tag) {
    return xmlNewDocNode(doc_, nullptr, BAD_CAST tag.c_str(), nullptr);
}

Node Dom::createText(const std::string& text) {
    return xmlNewDocText(doc_, BAD_CAST text.c_str());
}

Node Dom::copyNode(Node n) { return xmlDocCopyNode(n, doc_, 1); }

void insertBefore(Node parent, Node n, Node ref) {
    xmlUnlinkNode(n);
    n->parent = parent;
    if (ref) {
        n->next = ref;
        n->prev = ref->prev;
        if (ref->prev) ref->prev->next = n;
        else parent->children = n;
        ref->prev = n;
    } else {
        n->next = nullptr;
        n->prev = parent->last;
        if (parent->last) parent->last->next = n;
        else parent->children = n;
        parent->last = n;
    }
}

// Map windows-1252 bytes to UTF-8. Browsers use this table for latin-1 too.
static std::string cp1252ToUtf8(const std::string& in) {
    static const unsigned short hi[32] = {
        0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
        0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
    std::string out;
    out.reserve(in.size() + in.size() / 8);
    for (unsigned char c : in) {
        unsigned cp = c < 0x80 ? c : (c < 0xA0 ? hi[c - 0x80] : c);
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// True when the bytes are valid UTF-8 (control characters do not count).
static bool isUtf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    size_t n = s.size();
    for (size_t i = 0; i < n;) {
        unsigned c = p[i];
        size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (!len || i + len > n) return false;
        for (size_t k = 1; k < len; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        if (len == 2 && c < 0xC2) return false;
        i += len;
    }
    return true;
}

// Find the charset of a page: BOM, then <meta charset>, then a UTF-8 check.
static std::string sniffCharset(const std::string& html) {
    if (html.size() >= 3 && html.compare(0, 3, "\xEF\xBB\xBF") == 0) return "UTF-8";
    if (html.size() >= 2 && html.compare(0, 2, "\xFF\xFE") == 0) return "UTF-16LE";
    if (html.size() >= 2 && html.compare(0, 2, "\xFE\xFF") == 0) return "UTF-16BE";
    std::string head = toLower(html.substr(0, 4096));
    size_t pos = 0;
    while ((pos = head.find("<meta", pos)) != std::string::npos) {
        size_t end = head.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = head.substr(pos, end - pos);
        size_t cs = tag.find("charset");
        if (cs != std::string::npos) {
            size_t i = cs + 7;
            while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
            if (i < tag.size() && tag[i] == '=') {
                ++i;
                while (i < tag.size() && (tag[i] == ' ' || tag[i] == '"' || tag[i] == '\'')) ++i;
                size_t j = i;
                while (j < tag.size() && (std::isalnum(static_cast<unsigned char>(tag[j])) || tag[j] == '-' || tag[j] == '_' || tag[j] == ':' || tag[j] == '.')) ++j;
                std::string name = tag.substr(i, j - i);
                if (!name.empty()) {
                    if (name == "utf8" || name == "utf-8" || name == "unicode-1-1-utf-8") return "UTF-8";
                    if (name == "iso-8859-1" || name == "latin1" || name == "us-ascii" || name == "ascii" || name == "windows-1252" || name == "cp1252")
                        return "windows-1252";
                    return name;
                }
            }
        }
        pos = end;
    }
    return isUtf8(html) ? "UTF-8" : "windows-1252";
}

static const int kParseOpts = HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET;

static xmlDoc* readHtml(const std::string& bytes, const char* enc) {
    return htmlReadMemory(bytes.data(), static_cast<int>(bytes.size()), nullptr, enc, kParseOpts);
}

std::unique_ptr<Dom> parseDocument(const std::string& html) {
    std::string cs = sniffCharset(html);
    xmlDoc* d = nullptr;
    if (cs == "windows-1252") {
        // Convert ourselves. This does not need iconv.
        d = readHtml(cp1252ToUtf8(html), "UTF-8");
    } else {
        d = readHtml(html, cs.c_str());
        if (!d) d = readHtml(html, "UTF-8");
    }
    if (!d) d = htmlNewDocNoDtD(nullptr, nullptr);
    auto dom = std::make_unique<Dom>(d);
    dom->body();
    return dom;
}

std::unique_ptr<Dom> parseFragment(const std::string& html) {
    std::string wrapped = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>" + html + "</body></html>";
    xmlDoc* d = readHtml(wrapped, "UTF-8");
    if (!d) d = htmlNewDocNoDtD(nullptr, nullptr);
    auto dom = std::make_unique<Dom>(d);
    dom->body();
    return dom;
}

bool isElement(Node n) { return n && n->type == XML_ELEMENT_NODE; }
bool isText(Node n) { return n && (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE); }

std::string tagName(Node n) {
    if (!isElement(n) || !n->name) return "";
    return toLower(reinterpret_cast<const char*>(n->name));
}

bool isTag(Node n, const char* tag) {
    return isElement(n) && n->name && xmlStrcasecmp(n->name, BAD_CAST tag) == 0;
}

int headingLevelOfTag(Node n) {
    if (!isElement(n) || !n->name) return 0;
    const char* nm = reinterpret_cast<const char*>(n->name);
    if ((nm[0] == 'h' || nm[0] == 'H') && nm[1] >= '1' && nm[1] <= '6' && nm[2] == 0) return nm[1] - '0';
    return 0;
}

bool isHeading(Node n) { return headingLevelOfTag(n) > 0; }

bool hasAttr(Node n, const char* name) {
    return isElement(n) && xmlHasProp(n, BAD_CAST name) != nullptr;
}

std::string attr(Node n, const char* name) {
    if (!isElement(n)) return "";
    xmlChar* v = xmlGetProp(n, BAD_CAST name);
    if (!v) return "";
    std::string out(reinterpret_cast<const char*>(v));
    xmlFree(v);
    return out;
}

void setAttr(Node n, const char* name, const std::string& value) {
    xmlSetProp(n, BAD_CAST name, BAD_CAST value.c_str());
}

void removeAttr(Node n, const char* name) {
    xmlAttr* a = xmlHasProp(n, BAD_CAST name);
    if (a) xmlRemoveProp(a);
}

void keepOnlyAttrs(Node n, const std::set<std::string>& keep) {
    xmlAttr* a = n->properties;
    while (a) {
        xmlAttr* next = a->next;
        std::string nm = a->name ? toLower(reinterpret_cast<const char*>(a->name)) : "";
        if (!keep.count(nm) || a->ns) xmlRemoveProp(a);
        a = next;
    }
}

std::vector<std::string> classList(Node n) { return splitWords(attr(n, "class")); }

bool hasClass(Node n, const std::string& cls) {
    for (const auto& c : classList(n))
        if (c == cls) return true;
    return false;
}

std::string textContent(Node n) {
    if (!n) return "";
    xmlChar* c = xmlNodeGetContent(n);
    if (!c) return "";
    std::string out(reinterpret_cast<const char*>(c));
    xmlFree(c);
    return out;
}

void setTextContent(Dom& dom, Node n, const std::string& text) {
    while (n->children) dom.remove(n->children);
    appendChild(n, dom.createText(text));
}

bool isConnected(Node n) {
    for (Node p = n; p; p = p->parent)
        if (p->type == XML_DOCUMENT_NODE || p->type == XML_HTML_DOCUMENT_NODE) return true;
    return false;
}

bool contains(Node ancestor, Node n) {
    for (Node p = n; p; p = p->parent)
        if (p == ancestor) return true;
    return false;
}

Node parentElement(Node n) {
    return n && n->parent && n->parent->type == XML_ELEMENT_NODE ? n->parent : nullptr;
}

Node firstElementChild(Node n) {
    for (Node c = n ? n->children : nullptr; c; c = c->next)
        if (isElement(c)) return c;
    return nullptr;
}

std::vector<Node> childNodes(Node n) {
    std::vector<Node> out;
    for (Node c = n ? n->children : nullptr; c; c = c->next) out.push_back(c);
    return out;
}

std::vector<Node> elementChildren(Node n) {
    std::vector<Node> out;
    for (Node c = n ? n->children : nullptr; c; c = c->next)
        if (isElement(c)) out.push_back(c);
    return out;
}

static void collectElements(Node n, std::vector<Node>& out) {
    for (Node c = n->children; c; c = c->next) {
        if (!isElement(c)) continue;
        out.push_back(c);
        collectElements(c, out);
    }
}

std::vector<Node> descendants(Node root) {
    std::vector<Node> out;
    if (root) collectElements(root, out);
    return out;
}

// HTML serialization without added line breaks (format 0).
std::string outerHtml(Node n) {
    std::string out;
    if (!n) return out;
    xmlOutputBuffer* buf = xmlAllocOutputBuffer(nullptr);
    if (!buf) return out;
    htmlNodeDumpFormatOutput(buf, n->doc, n, "UTF-8", 0);
    xmlOutputBufferFlush(buf);
    const xmlChar* content = xmlOutputBufferGetContent(buf);
    if (content) out.assign(reinterpret_cast<const char*>(content), xmlOutputBufferGetSize(buf));
    xmlOutputBufferClose(buf);
    return out;
}

std::string innerHtml(Node n) {
    std::string out;
    if (!n) return out;
    for (Node c = n->children; c; c = c->next) out += outerHtml(c);
    return out;
}

static bool isVoidTag(const std::string& t) {
    static const std::set<std::string> v = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"};
    return v.count(t) > 0;
}

static bool isXmlName(const xmlChar* s) {
    if (!s || !*s) return false;
    auto ok0 = [](unsigned char c) { return std::isalpha(c) || c == '_' || c >= 0x80; };
    auto ok = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '.' || c >= 0x80; };
    if (!ok0(s[0])) return false;
    for (const xmlChar* p = s + 1; *p; ++p)
        if (!ok(*p)) return false;
    return true;
}

// Make a copied subtree safe for the XML serializer: no comments, valid
// names, valid characters, and no self-closed non-void elements.
static void fixForXml(xmlDoc* doc, Node n) {
    Node c = n->children;
    while (c) {
        Node next = c->next;
        if (c->type == XML_COMMENT_NODE || c->type == XML_PI_NODE || c->type == XML_DTD_NODE || c->type == XML_ENTITY_REF_NODE) {
            xmlUnlinkNode(c);
            xmlFreeNode(c);
        } else if (c->type == XML_TEXT_NODE || c->type == XML_CDATA_SECTION_NODE) {
            std::string t = cleanXmlText(c->content ? reinterpret_cast<const char*>(c->content) : "");
            Node txt = xmlNewDocText(doc, BAD_CAST t.c_str());
            insertBefore(n, txt, c);
            xmlUnlinkNode(c);
            xmlFreeNode(c);
        } else if (c->type == XML_ELEMENT_NODE) {
            if (!isXmlName(c->name)) xmlNodeSetName(c, BAD_CAST "span");
            xmlAttr* a = c->properties;
            while (a) {
                xmlAttr* an = a->next;
                if (!isXmlName(a->name) || a->ns) {
                    xmlRemoveProp(a);
                } else {
                    xmlChar* raw = xmlGetProp(c, a->name);
                    std::string rs = raw ? reinterpret_cast<const char*>(raw) : "";
                    if (raw) xmlFree(raw);
                    std::string cleaned = cleanXmlText(rs);
                    if (cleaned != rs) xmlSetProp(c, a->name, BAD_CAST cleaned.c_str());
                }
                a = an;
            }
            fixForXml(doc, c);
            if (!c->children && !isVoidTag(tagName(c))) {
                // An empty text child makes libxml2 write <p></p>, not <p/>.
                appendChild(c, xmlNewDocText(doc, BAD_CAST ""));
            }
        } else {
            xmlUnlinkNode(c);
            xmlFreeNode(c);
        }
        c = next;
    }
}

std::string serializeXhtml(Node parent) {
    std::string out;
    if (!parent) return out;
    xmlDoc* x = xmlNewDoc(BAD_CAST "1.0");
    x->encoding = xmlStrdup(BAD_CAST "UTF-8");
    Node holder = xmlNewDocNode(x, nullptr, BAD_CAST "div", nullptr);
    xmlDocSetRootElement(x, holder);
    for (Node c = parent->children; c; c = c->next) {
        Node cp = xmlDocCopyNode(c, x, 1);
        if (cp) appendChild(holder, cp);
    }
    fixForXml(x, holder);
    xmlBuffer* buf = xmlBufferCreate();
    for (Node c = holder->children; c; c = c->next) xmlNodeDump(buf, x, c, 0, 0);
    out.assign(reinterpret_cast<const char*>(xmlBufferContent(buf)), static_cast<size_t>(xmlBufferLength(buf)));
    xmlBufferFree(buf);
    xmlFreeDoc(x);
    return out;
}

// ============================================================= selectors

namespace {
struct SelLexer {
    const std::string& s;
    size_t i = 0;
    bool eof() const { return i >= s.size(); }
    char peek() const { return eof() ? '\0' : s[i]; }
    void skipWs() {
        while (!eof() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    std::string ident() {
        size_t st = i;
        while (!eof() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '_' || s[i] == ':' || (s[i] == '*' && i == st))) ++i;
        return s.substr(st, i - st);
    }
    std::string value() {
        skipWs();
        if (peek() == '"' || peek() == '\'') {
            char q = s[i++];
            size_t st = i;
            while (!eof() && s[i] != q) ++i;
            std::string v = s.substr(st, i - st);
            if (!eof()) ++i;
            return v;
        }
        return ident();
    }
};
} // namespace

Selector::Selector(const std::string& text) {
    SelLexer lx{text};
    Complex cur;
    Compound comp;
    bool compHas = false;
    auto endCompound = [&]() {
        if (compHas) cur.push_back(comp);
        comp = Compound{};
        compHas = false;
    };
    while (!lx.eof()) {
        char c = lx.peek();
        if (c == ',') {
            endCompound();
            if (!cur.empty()) list_.push_back(cur);
            cur.clear();
            ++lx.i;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            endCompound();
            lx.skipWs();
        } else if (c == '#') {
            ++lx.i;
            comp.ids.push_back(lx.ident());
            compHas = true;
        } else if (c == '.') {
            ++lx.i;
            comp.classes.push_back(lx.ident());
            compHas = true;
        } else if (c == '[') {
            ++lx.i;
            lx.skipWs();
            AttrTest t;
            t.name = toLower(lx.ident());
            lx.skipWs();
            if (lx.peek() == '*' || lx.peek() == '^' || lx.peek() == '$' || lx.peek() == '~') {
                t.op = std::string(1, lx.peek());
                ++lx.i;
            }
            if (lx.peek() == '=') {
                ++lx.i;
                t.op += "=";
                t.value = lx.value();
                lx.skipWs();
                if (lx.peek() == 'i' || lx.peek() == 'I') {
                    t.icase = true;
                    ++lx.i;
                }
            }
            while (!lx.eof() && lx.peek() != ']') ++lx.i;
            if (!lx.eof()) ++lx.i;
            comp.attrs.push_back(t);
            compHas = true;
        } else {
            std::string id = lx.ident();
            if (id.empty()) {
                ++lx.i;
                continue;
            }
            comp.tag = id == "*" ? "" : toLower(id);
            compHas = true;
        }
    }
    endCompound();
    if (!cur.empty()) list_.push_back(cur);
}

bool Selector::matchCompound(const Compound& c, Node n) {
    if (!isElement(n)) return false;
    if (!c.tag.empty() && tagName(n) != c.tag) return false;
    if (!c.ids.empty()) {
        std::string id = attr(n, "id");
        for (const auto& i : c.ids)
            if (id != i) return false;
    }
    if (!c.classes.empty()) {
        auto cl = classList(n);
        for (const auto& want : c.classes)
            if (std::find(cl.begin(), cl.end(), want) == cl.end()) return false;
    }
    for (const auto& t : c.attrs) {
        if (!hasAttr(n, t.name.c_str())) return false;
        if (t.op.empty()) continue;
        std::string v = attr(n, t.name.c_str());
        std::string want = t.value;
        if (t.icase) {
            v = toLower(v);
            want = toLower(want);
        }
        if (t.op == "=" && v != want) return false;
        if (t.op == "*=" && (want.empty() || v.find(want) == std::string::npos)) return false;
        if (t.op == "^=" && (want.empty() || v.rfind(want, 0) != 0)) return false;
        if (t.op == "$=" && (want.empty() || v.size() < want.size() || v.compare(v.size() - want.size(), want.size(), want) != 0)) return false;
        if (t.op == "~=") {
            auto words = splitWords(v);
            if (std::find(words.begin(), words.end(), want) == words.end()) return false;
        }
    }
    return true;
}

bool Selector::matchComplex(const Complex& c, Node n) const {
    if (c.empty() || !matchCompound(c.back(), n)) return false;
    Node p = parentElement(n);
    for (size_t k = c.size() - 1; k-- > 0;) {
        while (p && !matchCompound(c[k], p)) p = parentElement(p);
        if (!p) return false;
        p = parentElement(p);
    }
    return true;
}

bool Selector::matches(Node n) const {
    for (const auto& c : list_)
        if (matchComplex(c, n)) return true;
    return false;
}

std::vector<Node> querySelectorAll(Node root, const Selector& sel) {
    std::vector<Node> out;
    for (Node n : descendants(root))
        if (sel.matches(n)) out.push_back(n);
    return out;
}

std::vector<Node> querySelectorAll(Node root, const std::string& sel) {
    return querySelectorAll(root, Selector(sel));
}

static Node firstMatch(Node n, const Selector& sel) {
    for (Node c = n->children; c; c = c->next) {
        if (!isElement(c)) continue;
        if (sel.matches(c)) return c;
        if (Node r = firstMatch(c, sel)) return r;
    }
    return nullptr;
}

Node querySelector(Node root, const Selector& sel) { return root ? firstMatch(root, sel) : nullptr; }
Node querySelector(Node root, const std::string& sel) { return querySelector(root, Selector(sel)); }

Node closest(Node n, const std::string& sel) {
    Selector s(sel);
    for (Node p = n; p && isElement(p); p = parentElement(p))
        if (s.matches(p)) return p;
    return nullptr;
}

} // namespace ks::extract::detail
