// Body preparation and safe DOM copy (port of converters/prepare.ts).
// Every output format starts here:
// 1. prepareBody() parses Article.bodyHtml with libxml2. It applies the
//    table and link settings and makes all ids valid and unique.
// 2. copyNode() copies the prepared nodes into a target document. It keeps
//    only allowed tags and attributes. So the output is always well-formed.
#include "output/internal.h"

#include <libxml/HTMLparser.h>
#include <libxml/parser.h>
#include <libxml/xmlsave.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <set>

namespace ks::output::detail {

namespace {

const std::set<std::string> kDrop = {
    "script", "style", "iframe", "object", "embed", "noscript", "template", "form", "input",
    "button", "select", "textarea", "option", "svg", "math", "video", "audio", "canvas", "map",
    "area", "source", "track", "head", "title", "meta", "link", "base", "nav", "dialog", "frame",
    "frameset", "param", "applet",
};

// Tags that we copy with the same name.
const std::set<std::string> kKeep = {
    "h1", "h2", "h3", "h4", "h5", "h6", "p", "ul", "ol", "li", "blockquote", "pre", "code",
    "b", "strong", "i", "em", "u", "del", "sub", "sup", "span", "div", "section",
    "table", "thead", "tbody", "tfoot", "tr", "th", "td", "caption", "dl", "dt", "dd",
    "ruby", "rt", "rp",
};

// Tags that we copy with a different name. CrossPoint knows the target names.
const std::map<std::string, std::string> kRename = {
    {"article", "div"}, {"main", "div"}, {"header", "div"}, {"footer", "div"}, {"center", "div"},
    {"address", "div"}, {"details", "div"}, {"summary", "p"}, {"hgroup", "div"}, {"aside", "div"},
    {"cite", "i"}, {"dfn", "i"}, {"var", "i"}, {"q", "span"}, {"kbd", "code"}, {"samp", "code"},
    {"tt", "code"}, {"s", "del"}, {"strike", "del"}, {"ins", "u"}, {"mark", "span"}, {"small", "span"},
    {"big", "span"}, {"abbr", "span"}, {"time", "span"}, {"font", "span"}, {"bdi", "span"},
    {"bdo", "span"}, {"data", "span"}, {"label", "span"},
};

// Parent tags that can hold a <p>. In other parents we use <span>.
const std::set<std::string> kBlockParents = {
    "body", "div", "section", "blockquote", "li", "td", "th", "dd", "figure", "caption", "main",
    "header", "article",
};

const std::set<std::string> kBlockTags = {
    "p", "div", "section", "ul", "ol", "li", "h1", "h2", "h3", "h4", "h5", "h6", "blockquote",
    "pre", "table", "dl", "figure", "hr", "article", "main", "header", "footer", "aside",
};

// Common HTML entities. The XML parser knows only the five XML entities.
const char* kEntityDtd =
    "<!DOCTYPE div [\n"
    "<!ENTITY nbsp \"&#160;\"><!ENTITY shy \"&#173;\"><!ENTITY copy \"&#169;\"><!ENTITY reg \"&#174;\">"
    "<!ENTITY trade \"&#8482;\"><!ENTITY ndash \"&#8211;\"><!ENTITY mdash \"&#8212;\">"
    "<!ENTITY lsquo \"&#8216;\"><!ENTITY rsquo \"&#8217;\"><!ENTITY sbquo \"&#8218;\">"
    "<!ENTITY ldquo \"&#8220;\"><!ENTITY rdquo \"&#8221;\"><!ENTITY bdquo \"&#8222;\">"
    "<!ENTITY hellip \"&#8230;\"><!ENTITY middot \"&#183;\"><!ENTITY bull \"&#8226;\">"
    "<!ENTITY laquo \"&#171;\"><!ENTITY raquo \"&#187;\"><!ENTITY deg \"&#176;\">"
    "<!ENTITY times \"&#215;\"><!ENTITY divide \"&#247;\"><!ENTITY minus \"&#8722;\">"
    "<!ENTITY euro \"&#8364;\"><!ENTITY pound \"&#163;\"><!ENTITY yen \"&#165;\"><!ENTITY cent \"&#162;\">"
    "<!ENTITY sect \"&#167;\"><!ENTITY para \"&#182;\"><!ENTITY dagger \"&#8224;\">"
    "<!ENTITY thinsp \"&#8201;\"><!ENTITY ensp \"&#8194;\"><!ENTITY emsp \"&#8195;\">"
    "<!ENTITY zwj \"&#8205;\"><!ENTITY zwnj \"&#8204;\"><!ENTITY larr \"&#8592;\"><!ENTITY rarr \"&#8594;\">"
    "]>\n";

bool validId(const std::string& id) {
    // ^[A-Za-z_][A-Za-z0-9_.-]*$
    if (id.empty()) return false;
    unsigned char c0 = static_cast<unsigned char>(id[0]);
    if (!(std::isalpha(c0) || c0 == '_') || c0 >= 0x80) return false;
    for (unsigned char c : id)
        if (c >= 0x80 || !(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
}

std::string fixId(const std::string& raw) {
    std::string t = trim(raw);
    std::string id;
    bool run = false;
    for (unsigned char c : t) {
        bool ok = c < 0x80 && (std::isalnum(c) || c == '_' || c == '.' || c == '-');
        if (ok) {
            if (run) id += '-';
            run = false;
            id += static_cast<char>(c);
        } else {
            run = true;
        }
    }
    if (run) id += '-';
    size_t a = 0, b = id.size();
    while (a < b && id[a] == '-') ++a;
    while (b > a && id[b - 1] == '-') --b;
    id = id.substr(a, b - a);
    if (id.empty()) id = "id";
    if (!validId(id)) id = "id-" + id;
    return id;
}

// decodeURIComponent(). Returns the raw text when it is not valid.
std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size() || !std::isxdigit(static_cast<unsigned char>(s[i + 1])) ||
                !std::isxdigit(static_cast<unsigned char>(s[i + 2])))
                return s;
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    // Invalid UTF-8 after decode: JS throws. Keep the raw text then.
    if (toU8(toU32(out)) != out) return s;
    return out;
}

void freeNode(xmlNodePtr n) {
    xmlUnlinkNode(n);
    xmlFreeNode(n);
}

void unwrap(xmlNodePtr el) {
    if (!el->parent) return;
    while (el->children) {
        xmlNodePtr c = el->children;
        xmlUnlinkNode(c);
        xmlAddPrevSibling(el, c);
    }
    freeNode(el);
}

bool hasBlockChild(xmlNodePtr el) {
    for (xmlNodePtr c = el->children; c; c = c->next)
        if (isElement(c) && kBlockTags.count(nameOf(c))) return true;
    return false;
}

void removeDropped(xmlNodePtr n) {
    xmlNodePtr c = n->children;
    while (c) {
        xmlNodePtr next = c->next;
        if (isElement(c) && kDrop.count(nameOf(c))) freeNode(c);
        else if (c->type == XML_COMMENT_NODE || c->type == XML_PI_NODE) freeNode(c);
        else if (isElement(c)) removeDropped(c);
        c = next;
    }
}

// Rows of a table, like HTMLTableElement.rows (direct rows and rows of
// thead/tbody/tfoot, in tree order).
std::vector<xmlNodePtr> tableRows(xmlNodePtr table) {
    std::vector<xmlNodePtr> rows;
    for (xmlNodePtr c = table->children; c; c = c->next) {
        std::string n = nameOf(c);
        if (n == "tr") rows.push_back(c);
        else if (n == "thead" || n == "tbody" || n == "tfoot")
            for (xmlNodePtr r = c->children; r; r = r->next)
                if (nameOf(r) == "tr") rows.push_back(r);
    }
    return rows;
}

// Replaces a table with one paragraph per row. Cells are joined with " | ".
void linearizeTable(xmlNodePtr table) {
    xmlDocPtr doc = table->doc;
    xmlNsPtr ns = table->ns;
    xmlNodePtr box = newEl(doc, ns, "div", "table-linear");
    for (xmlNodePtr c = table->children; c; c = c->next) {
        if (nameOf(c) != "caption") continue;
        std::string cap = collapseWs(textOf(c));
        if (!cap.empty()) {
            xmlNodePtr p = addEl(box, ns, "p");
            xmlNodePtr b = addEl(p, ns, "strong");
            addText(b, cap);
        }
        break;
    }
    for (xmlNodePtr row : tableRows(table)) {
        xmlNodePtr p = newEl(doc, ns, "p");
        bool first = true;
        for (xmlNodePtr cell : childElements(row)) {
            std::string cn = nameOf(cell);
            if (cn != "td" && cn != "th") continue;
            std::string text = collapseWs(textOf(cell));
            auto imgs = descendants(cell, [](xmlNodePtr e) { return nameOf(e) == "img"; });
            if (text.empty() && imgs.empty()) continue;
            if (!first) addText(p, " | ");
            first = false;
            xmlNodePtr holder = cn == "th" ? addEl(p, ns, "strong") : p;
            if (hasBlockChild(cell)) {
                addText(holder, text);
                for (xmlNodePtr im : imgs) xmlAddChild(holder, xmlDocCopyNode(im, doc, 2));
            } else {
                while (cell->children) {
                    xmlNodePtr k = cell->children;
                    xmlUnlinkNode(k);
                    xmlAddChild(holder, k);
                }
            }
        }
        if (p->children) xmlAddChild(box, p);
        else xmlFreeNode(p);
    }
    xmlReplaceNode(table, box);
    xmlFreeNode(table);
}

// Makes a "<p><b>text</b></p>" from a <dt>. The dt content moves into the <b>.
xmlNodePtr dtToParagraph(xmlNodePtr dt) {
    xmlNodePtr p = newEl(dt->doc, dt->ns, "p");
    xmlNodePtr b = addEl(p, dt->ns, "b");
    while (dt->children) {
        xmlNodePtr k = dt->children;
        xmlUnlinkNode(k);
        xmlAddChild(b, k);
    }
    return p;
}

bool hasDirectDtDd(xmlNodePtr el) {
    for (xmlNodePtr c = el->children; c; c = c->next)
        if (nameOf(c) == "dt" || nameOf(c) == "dd") return true;
    return false;
}

// Makes a <dl> valid for XHTML: each group is dt+ dd+ (port of the web
// fixDefinitionList). Wikipedia uses "<dl><dt>" without <dd> as a pseudo-heading.
// - A dl without a dd becomes one "<p><b>dt text</b></p>" per dt.
// - A dd without a dt before it gets an empty dt.
// - Trailing dt elements without a dd become paragraphs after the dl.
// Other children of the dl move out of it, in place.
void fixDefinitionList(xmlNodePtr dl) {
    if (!dl->parent) return;
    std::vector<xmlNodePtr> kids = childElements(dl);
    bool anyDd = std::any_of(kids.begin(), kids.end(), [](xmlNodePtr k) { return nameOf(k) == "dd"; });
    if (!anyDd) {
        for (xmlNodePtr k : childNodes(dl)) {
            xmlUnlinkNode(k);
            if (nameOf(k) == "dt") {
                xmlAddPrevSibling(dl, dtToParagraph(k));
                xmlFreeNode(k);
            } else {
                xmlAddPrevSibling(dl, k);
            }
        }
        freeNode(dl);
        return;
    }
    enum { None, Dt, Dd } prev = None;
    std::vector<xmlNodePtr> trailing;
    for (xmlNodePtr k : kids) {
        std::string tag = nameOf(k);
        if (tag == "dt") {
            if (prev != Dt) trailing.clear();
            trailing.push_back(k);
            prev = Dt;
        } else if (tag == "dd") {
            if (prev == None) xmlAddPrevSibling(k, newEl(dl->doc, k->ns, "dt"));
            trailing.clear();
            prev = Dd;
        } else if (tag == "div" && hasDirectDtDd(k)) {
            // An HTML div group: keep it, but check it as its own list.
            prev = None;
            trailing.clear();
        } else {
            // Not allowed in a dl. Move it before the dl.
            xmlUnlinkNode(k);
            xmlAddPrevSibling(dl, k);
        }
    }
    if (prev == Dt) {
        xmlNodePtr at = dl;
        for (xmlNodePtr dt : trailing) {
            xmlNodePtr p = dtToParagraph(dt);
            xmlAddNextSibling(at, p);
            at = p;
            freeNode(dt);
        }
    }
    // Loose text in a dl is not allowed. Move it out.
    for (xmlNodePtr n : childNodes(dl)) {
        if (isText(n) && !trim(textOf(n)).empty()) {
            xmlUnlinkNode(n);
            xmlAddPrevSibling(dl, n);
        }
    }
    if (childElements(dl).empty()) freeNode(dl);
}

bool hasClassToken(xmlNodePtr n, const std::string& token) {
    std::string cls = attrOf(n, "class");
    size_t i = 0;
    while (i < cls.size()) {
        while (i < cls.size() && std::isspace(static_cast<unsigned char>(cls[i]))) ++i;
        size_t j = i;
        while (j < cls.size() && !std::isspace(static_cast<unsigned char>(cls[j]))) ++j;
        if (j > i && cls.compare(i, j - i, token) == 0) return true;
        i = j;
    }
    return false;
}

void initLibxml() {
    static std::once_flag once;
    std::call_once(once, [] { xmlInitParser(); });
}

xmlNodePtr findBody(xmlNodePtr n) {
    for (; n; n = n->next) {
        if (nameOf(n) == "body") return n;
        if (xmlNodePtr b = findBody(n->children)) return b;
    }
    return nullptr;
}

} // namespace

// ----------------------------------------------------------------- node helpers

bool isElement(xmlNodePtr n) { return n && n->type == XML_ELEMENT_NODE; }
bool isText(xmlNodePtr n) { return n && (n->type == XML_TEXT_NODE || n->type == XML_CDATA_SECTION_NODE); }

std::string nameOf(xmlNodePtr n) {
    if (!isElement(n) || !n->name) return "";
    return toLower(reinterpret_cast<const char*>(n->name));
}

std::string textOf(xmlNodePtr n) {
    if (!n) return "";
    xmlChar* c = xmlNodeGetContent(n);
    if (!c) return "";
    std::string s = reinterpret_cast<const char*>(c);
    xmlFree(c);
    return s;
}

std::string attrOf(xmlNodePtr n, const char* name) {
    if (!isElement(n)) return "";
    xmlChar* v = xmlGetProp(n, BAD_CAST name);
    if (!v) return "";
    std::string s = reinterpret_cast<const char*>(v);
    xmlFree(v);
    return s;
}

bool hasAttr(xmlNodePtr n, const char* name) { return isElement(n) && xmlHasProp(n, BAD_CAST name) != nullptr; }

std::vector<xmlNodePtr> childNodes(xmlNodePtr n) {
    std::vector<xmlNodePtr> out;
    for (xmlNodePtr c = n ? n->children : nullptr; c; c = c->next) out.push_back(c);
    return out;
}

std::vector<xmlNodePtr> childElements(xmlNodePtr n) {
    std::vector<xmlNodePtr> out;
    for (xmlNodePtr c = n ? n->children : nullptr; c; c = c->next)
        if (isElement(c)) out.push_back(c);
    return out;
}

static void collect(xmlNodePtr n, const std::function<bool(xmlNodePtr)>& pred, std::vector<xmlNodePtr>& out) {
    for (xmlNodePtr c = n->children; c; c = c->next) {
        if (!isElement(c)) continue;
        if (pred(c)) out.push_back(c);
        collect(c, pred, out);
    }
}

std::vector<xmlNodePtr> descendants(xmlNodePtr n, const std::function<bool(xmlNodePtr)>& pred) {
    std::vector<xmlNodePtr> out;
    if (n) collect(n, pred, out);
    return out;
}

bool isHeading(xmlNodePtr n) {
    std::string s = nameOf(n);
    return s.size() == 2 && s[0] == 'h' && s[1] >= '1' && s[1] <= '6';
}

int headingLevel(xmlNodePtr n) { return isHeading(n) ? nameOf(n)[1] - '0' : 0; }

xmlNodePtr newEl(xmlDocPtr doc, xmlNsPtr ns, const char* tag, const char* cls) {
    xmlNodePtr e = xmlNewDocNode(doc, ns, BAD_CAST tag, nullptr);
    if (cls) xmlSetProp(e, BAD_CAST "class", BAD_CAST cls);
    return e;
}

xmlNodePtr addEl(xmlNodePtr parent, xmlNsPtr ns, const char* tag, const char* cls) {
    xmlNodePtr e = newEl(parent->doc, ns, tag, cls);
    xmlAddChild(parent, e);
    return e;
}

void addText(xmlNodePtr parent, const std::string& text) {
    if (text.empty()) return;
    // xmlAddChild may merge the new node into a previous text node.
    xmlAddChild(parent, xmlNewDocText(parent->doc, BAD_CAST text.c_str()));
}

// ----------------------------------------------------------------- parse + prepare

Doc parseFragment(const std::string& html, xmlNodePtr* root) {
    initLibxml();
    *root = nullptr;
    std::string xml = std::string(kEntityDtd) + "<div xmlns=\"" + kXhtmlNs + "\">" + html + "</div>";
    xmlDocPtr d = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "body.xhtml", "UTF-8",
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING | XML_PARSE_NOENT |
                                    XML_PARSE_NOCDATA);
    if (d) {
        *root = xmlDocGetRootElement(d);
        return Doc(d);
    }
    // Fallback: the tolerant HTML parser.
    std::string h = "<!DOCTYPE html><html><head><meta charset=\"utf-8\"></head><body>" + html + "</body></html>";
    d = htmlReadMemory(h.data(), static_cast<int>(h.size()), "body.html", "UTF-8",
                       HTML_PARSE_RECOVER | HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
    if (!d) {
        // Not expected. Return an empty document with a root element.
        d = xmlNewDoc(BAD_CAST "1.0");
        xmlNodePtr div = xmlNewDocNode(d, nullptr, BAD_CAST "div", nullptr);
        xmlDocSetRootElement(d, div);
        *root = div;
        return Doc(d);
    }
    *root = findBody(xmlDocGetRootElement(d));
    if (!*root) *root = xmlDocGetRootElement(d);
    return Doc(d);
}

PreparedBody prepareBody(const Article& a, const StyleSettings& style) {
    PreparedBody pb;
    xmlNodePtr body = nullptr;
    pb.doc = parseFragment(a.bodyHtml, &body);
    pb.root = body;
    if (!body) return pb;

    removeDropped(body);

    // Definition lists. Work from the last list, so inner lists go first.
    auto dls = descendants(body, [](xmlNodePtr e) { return nameOf(e) == "dl"; });
    std::reverse(dls.begin(), dls.end());
    for (xmlNodePtr dl : dls) fixDefinitionList(dl);

    // Tables. Work from the last table, so inner tables go first.
    auto tables = descendants(body, [](xmlNodePtr e) { return nameOf(e) == "table"; });
    std::reverse(tables.begin(), tables.end());
    for (xmlNodePtr t : tables) {
        if (style.tableHandling == TableHandling::Drop) freeNode(t);
        else if (style.tableHandling == TableHandling::Linearize) linearizeTable(t);
    }

    // Ids: valid and unique.
    std::set<std::string> used;
    std::map<std::string, std::string> remap;
    auto unique = [&](const std::string& base) {
        std::string id = base;
        int n = 2;
        while (used.count(id)) id = base + "-" + std::to_string(n++);
        used.insert(id);
        return id;
    };
    for (xmlNodePtr e : descendants(body, [](xmlNodePtr x) { return hasAttr(x, "id"); })) {
        std::string raw = attrOf(e, "id");
        std::string id = unique(fixId(raw));
        remap.emplace(raw, id);
        xmlSetProp(e, BAD_CAST "id", BAD_CAST id.c_str());
    }
    int sec = 0;
    for (xmlNodePtr h : descendants(body, isHeading)) {
        if (!attrOf(h, "id").empty()) continue;
        std::string id;
        do id = "sec-" + std::to_string(++sec);
        while (used.count(id));
        used.insert(id);
        xmlSetProp(h, BAD_CAST "id", BAD_CAST id.c_str());
    }

    // Links. Internal "#id" links stay, so the reader can jump inside the book.
    for (xmlNodePtr link : descendants(body, [](xmlNodePtr x) { return nameOf(x) == "a"; })) {
        std::string href = trim(attrOf(link, "href"));
        if (!href.empty() && href[0] == '#') {
            std::string frag = percentDecode(href.substr(1));
            std::string target;
            if (auto it = remap.find(frag); it != remap.end()) target = it->second;
            else if (used.count(frag)) target = frag;
            if (!target.empty()) xmlSetProp(link, BAD_CAST "href", BAD_CAST("#" + target).c_str());
            else unwrap(link);
            continue;
        }
        if (!style.keepLinks) unwrap(link);
    }

    // Remove single wrappers.
    xmlNodePtr root = body;
    for (;;) {
        auto kids = childElements(root);
        bool loose = false;
        for (xmlNodePtr c = root->children; c; c = c->next)
            if (isText(c) && !trim(textOf(c)).empty()) loose = true;
        if (kids.size() != 1 || loose) break;
        std::string n = nameOf(kids[0]);
        if (n != "div" && n != "section" && n != "article" && n != "main") break;
        root = kids[0];
    }
    pb.root = root;
    return pb;
}

// ----------------------------------------------------------------- safe copy

static bool blockParent(xmlNodePtr parent) { return kBlockParents.count(nameOf(parent)) > 0; }

static void copyId(xmlNodePtr src, xmlNodePtr out) {
    std::string id = attrOf(src, "id");
    if (!id.empty() && validId(id)) xmlSetProp(out, BAD_CAST "id", BAD_CAST id.c_str());
}

static void copySpan(xmlNodePtr src, xmlNodePtr out) {
    for (const char* name : {"colspan", "rowspan"}) {
        std::string v = attrOf(src, name);
        if (v.empty() || v.size() > 3) continue;
        bool digits = std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits && std::stoi(v) > 0) xmlSetProp(out, BAD_CAST name, BAD_CAST v.c_str());
    }
}

static void copyChildren(xmlNodePtr src, xmlNodePtr out, xmlNsPtr ns, const CopyContext& ctx) {
    for (xmlNodePtr c = src->children; c; c = c->next) copyNode(c, out, ns, ctx);
}

void copyNode(xmlNodePtr node, xmlNodePtr outParent, xmlNsPtr ns, const CopyContext& ctx) {
    if (!node) return;
    if (isText(node)) {
        addText(outParent, cleanXmlText(node->content ? reinterpret_cast<const char*>(node->content) : ""));
        return;
    }
    if (node->type == XML_ENTITY_REF_NODE) {
        addText(outParent, cleanXmlText(textOf(node)));
        return;
    }
    if (!isElement(node)) return;
    const std::string tag = nameOf(node);
    if (kDrop.count(tag)) return;
    xmlDocPtr doc = outParent->doc;

    if (tag == "img") {
        std::string alt = collapseWs(attrOf(node, "alt"));
        auto out = ctx.image ? ctx.image(trim(attrOf(node, "src")), alt) : std::nullopt;
        if (out) {
            xmlNodePtr img = addEl(outParent, ns, "img");
            xmlSetProp(img, BAD_CAST "src", BAD_CAST out->c_str());
            xmlSetProp(img, BAD_CAST "alt", BAD_CAST alt.c_str());
            copyId(node, img);
        } else if (!alt.empty()) {
            xmlNodePtr holder = addEl(outParent, ns, blockParent(outParent) ? "p" : "span", "img-alt");
            addText(holder, alt);
        }
        return;
    }
    if (tag == "br" || tag == "hr") {
        addEl(outParent, ns, tag.c_str());
        return;
    }
    if (tag == "wbr") return;
    if (tag == "a") {
        auto href = ctx.href ? ctx.href(trim(attrOf(node, "href"))) : std::nullopt;
        if (!href) {
            copyChildren(node, outParent, ns, ctx);
            return;
        }
        xmlNodePtr a = addEl(outParent, ns, "a");
        xmlSetProp(a, BAD_CAST "href", BAD_CAST cleanXmlText(*href).c_str());
        copyId(node, a);
        // Keep the pack marker class so CSS can style links that stay online.
        if (hasClassToken(node, "ext")) xmlSetProp(a, BAD_CAST "class", BAD_CAST "ext");
        copyChildren(node, a, ns, ctx);
        return;
    }
    if (tag == "figure") {
        xmlNodePtr out = addEl(outParent, ns, "div", "figure");
        copyId(node, out);
        copyChildren(node, out, ns, ctx);
        return;
    }
    if (tag == "figcaption") {
        // A caption that holds a table or list cannot sit inside <p>. Use <div> then.
        const char* name = blockParent(outParent) ? (hasBlockChild(node) ? "div" : "p") : "span";
        xmlNodePtr out = addEl(outParent, ns, name, "caption");
        copyId(node, out);
        copyChildren(node, out, ns, ctx);
        return;
    }
    std::string name;
    if (kKeep.count(tag)) name = tag;
    else if (auto it = kRename.find(tag); it != kRename.end()) name = it->second;
    if (name.empty()) {
        // Unknown tag: keep its content only.
        copyChildren(node, outParent, ns, ctx);
        return;
    }
    xmlNodePtr out = newEl(doc, ns, name.c_str());
    std::string cls = attrOf(node, "class");
    // Keep only the classes that our own steps add.
    if (cls == "img-alt" || cls == "caption" || cls == "figure" || cls == "table-linear" || cls == "ext" || cls == "source")
        xmlSetProp(out, BAD_CAST "class", BAD_CAST cls.c_str());
    copyId(node, out);
    if (name == "td" || name == "th") copySpan(node, out);
    xmlAddChild(outParent, out);
    copyChildren(node, out, ns, ctx);
}

void copyNodes(const std::vector<xmlNodePtr>& nodes, xmlNodePtr outParent, xmlNsPtr ns, const CopyContext& ctx) {
    for (xmlNodePtr n : nodes) copyNode(n, outParent, ns, ctx);
}

std::optional<std::string> defaultHref(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    if (raw[0] == '#') return raw.size() > 1 ? std::optional<std::string>(raw) : std::nullopt;
    std::string l = toLower(raw.substr(0, 8));
    if (l.rfind("http:", 0) == 0 || l.rfind("https:", 0) == 0 || l.rfind("mailto:", 0) == 0) return raw;
    return std::nullopt;
}

void appendTitleBlock(xmlNodePtr parent, xmlNsPtr ns, const std::string& title, const std::vector<Line>& lines) {
    xmlNodePtr h = addEl(parent, ns, "h1", "title");
    addText(h, cleanXmlText(title.empty() ? "Untitled" : title));
    for (const Line& line : lines) {
        xmlNodePtr p = addEl(parent, ns, "p", "meta");
        xmlNodePtr b = addEl(p, ns, "b");
        addText(b, line.label + ": ");
        addText(p, cleanXmlText(line.value));
    }
}

void appendSourceNote(xmlNodePtr parent, xmlNsPtr ns, const std::string& text) {
    xmlNodePtr section = addEl(parent, ns, "section", "source-note");
    addEl(section, ns, "hr");
    xmlNodePtr p = addEl(section, ns, "p");
    addText(p, cleanXmlText(text));
}

// ----------------------------------------------------------------- render + serialize

void renderBodyInto(const Article& a, const BuildOptions& o, xmlNodePtr parent, const CopyContext* ctx) {
    PreparedBody pb = prepareBody(a, o.style);
    if (!pb.root) return;
    CopyContext def;
    if (!ctx) {
        // The HTML file keeps the original image URLs. data: URLs stay too.
        const bool include = o.includeImages;
        def.image = [include](const std::string& src, const std::string&) -> std::optional<std::string> {
            std::string l = toLower(src.substr(0, 11));
            if (include && (l.rfind("http:", 0) == 0 || l.rfind("https:", 0) == 0 || l.rfind("data:image/", 0) == 0))
                return src;
            return std::nullopt;
        };
        def.href = defaultHref;
        ctx = &def;
    }
    copyNodes(childNodes(pb.root), parent, parent->ns, *ctx);
}

Rendered renderBody(const Article& a, const BuildOptions& o, const CopyContext* ctx) {
    Rendered r;
    r.doc = Doc(xmlNewDoc(BAD_CAST "1.0"));
    r.container = xmlNewDocNode(r.doc.get(), nullptr, BAD_CAST "div", nullptr);
    xmlDocSetRootElement(r.doc.get(), r.container);
    renderBodyInto(a, o, r.container, ctx);
    return r;
}

static void escapeHtmlText(const std::string& s, std::string& out, bool attr) {
    for (char c : cleanXmlText(s)) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"':
                if (attr) out += "&quot;";
                else out += c;
                break;
            default: out += c;
        }
    }
}

void serializeHtml(xmlNodePtr n, std::string& out) {
    static const std::set<std::string> voids = {"area", "base", "br", "col", "embed", "hr", "img",
                                                "input", "link", "meta", "source", "track", "wbr"};
    if (isText(n)) {
        escapeHtmlText(n->content ? reinterpret_cast<const char*>(n->content) : "", out, false);
        return;
    }
    if (!isElement(n)) return;
    const std::string tag = nameOf(n);
    out += "<" + tag;
    for (xmlAttrPtr p = n->properties; p; p = p->next) {
        out += " ";
        if (p->ns && p->ns->prefix) out += std::string(reinterpret_cast<const char*>(p->ns->prefix)) + ":";
        out += reinterpret_cast<const char*>(p->name);
        out += "=\"";
        escapeHtmlText(textOf(reinterpret_cast<xmlNodePtr>(p)), out, true);
        out += "\"";
    }
    out += ">";
    if (voids.count(tag)) return;
    for (xmlNodePtr c = n->children; c; c = c->next) serializeHtml(c, out);
    out += "</" + tag + ">";
}

std::string serializeXml(xmlNodePtr n) {
    xmlBufferPtr buf = xmlBufferCreate();
    xmlSaveCtxtPtr sc = xmlSaveToBuffer(buf, "UTF-8", XML_SAVE_NO_DECL);
    std::string s;
    if (sc) {
        xmlSaveTree(sc, n);
        xmlSaveClose(sc);
        s.assign(reinterpret_cast<const char*>(xmlBufferContent(buf)), static_cast<size_t>(xmlBufferLength(buf)));
    }
    xmlBufferFree(buf);
    return s;
}

} // namespace ks::output::detail
