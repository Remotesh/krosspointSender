// Tests for the extract module. The expectations follow the web tests in
// ../tests/extractors/*.test.ts. bodyHtml is XHTML here, so void tags are
// self-closed ("<img .../>", "<br/>").
#include "check.h"
#include "extract/extract.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

using namespace ks;
using namespace ks::extract;

namespace {

std::string fixture(const std::string& name) {
    const char* dir = std::getenv("KS_FIXTURES");
    std::string path = std::string(dir ? dir : "../tests/fixtures") + "/" + name;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "Cannot open fixture %s\n", path.c_str());
        return "";
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }
bool matches(const std::string& s, const std::string& re) { return std::regex_search(s, std::regex(re)); }

// Parse the fragment with the XML parser (not the HTML parser).
xmlDoc* parseXml(const std::string& html) {
    std::string doc = "<root>" + html + "</root>";
    return xmlReadMemory(doc.data(), static_cast<int>(doc.size()), nullptr, "UTF-8", XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
}

bool wellFormed(const std::string& html) {
    xmlDoc* d = parseXml(html);
    if (!d) {
        std::fprintf(stderr, "Not well-formed XML:\n%s\n", html.substr(0, 2000).c_str());
        return false;
    }
    xmlFreeDoc(d);
    return true;
}

void collect(xmlNode* n, std::set<std::string>& tags, std::set<std::string>& attrs) {
    for (xmlNode* c = n->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        tags.insert(reinterpret_cast<const char*>(c->name));
        for (xmlAttr* a = c->properties; a; a = a->next) attrs.insert(reinterpret_cast<const char*>(a->name));
        collect(c, tags, attrs);
    }
}

void tagsAndAttrs(const std::string& html, std::set<std::string>& tags, std::set<std::string>& attrs) {
    xmlDoc* d = parseXml(html);
    if (!d) return;
    collect(xmlDocGetRootElement(d), tags, attrs);
    xmlFreeDoc(d);
}

int countOf(const std::string& s, const std::string& sub) {
    int n = 0;
    for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + 1)) ++n;
    return n;
}

// ------------------------------------------------------------- url

void testNormalizeUrl() {
    CHECK_EQ(normalizeUrl("  example.com/a  ").value_or(""), std::string("https://example.com/a"));
    CHECK_EQ(normalizeUrl("www.example.com").value_or(""), std::string("https://www.example.com/"));
    CHECK_EQ(normalizeUrl("//cdn.example.com/x").value_or(""), std::string("https://cdn.example.com/x"));
    CHECK_EQ(normalizeUrl("http://EXAMPLE.com/Path").value_or(""), std::string("http://example.com/Path"));
    CHECK(!normalizeUrl("javascript:alert(1)"));
    CHECK(!normalizeUrl("ftp://example.com/file"));
    CHECK(!normalizeUrl("mailto:a@b.c"));
    CHECK(!normalizeUrl("data:text/html,hi"));
    CHECK(!normalizeUrl(""));
    CHECK(!normalizeUrl("hello world"));
    CHECK(!normalizeUrl("notaurl"));
    CHECK_EQ(normalizeUrl("https://ex.com/a?utm_source=x&utm_medium=y&id=5&fbclid=abc").value_or(""), std::string("https://ex.com/a?id=5"));
    CHECK_EQ(normalizeUrl("https://ex.com/a?utm_source=x&gclid=1").value_or(""), std::string("https://ex.com/a"));
    CHECK_EQ(normalizeUrl("https://ex.com/a?utm_campaign=z#part").value_or(""), std::string("https://ex.com/a#part"));
    CHECK_EQ(normalizeUrl("localhost:5173/x").value_or(""), std::string("https://localhost:5173/x"));
    CHECK_EQ(normalizeUrl("http://192.168.1.50/").value_or(""), std::string("http://192.168.1.50/"));
    CHECK_EQ(normalizeUrl("https://example.com:443/a").value_or(""), std::string("https://example.com/a"));
}

void testWikipediaUrl() {
    CHECK(isWikipediaUrl("https://en.wikipedia.org/wiki/Sourdough"));
    CHECK(isWikipediaUrl("de.m.wikipedia.org/wiki/Brot"));
    CHECK(isWikipediaUrl("https://en.wikipedia.org/w/index.php?title=Bread"));
    CHECK(!isWikipediaUrl("https://wikipedia.org.evil.com/wiki/X"));
    CHECK(!isWikipediaUrl("https://example.com/wiki/X"));
    CHECK(!isWikipediaUrl("https://en.wikipedia.org/"));

    auto r = parseWikipediaUrl("https://en.wikipedia.org/wiki/E_Ink");
    CHECK(r.has_value());
    if (r) {
        CHECK_EQ(r->lang, std::string("en"));
        CHECK_EQ(r->title, std::string("E Ink"));
        CHECK_EQ(r->host, std::string("en.wikipedia.org"));
    }
    auto d = parseWikipediaUrl("https://de.m.wikipedia.org/wiki/Br%C3%B6tchen#Herstellung");
    CHECK(d.has_value());
    if (d) {
        CHECK_EQ(d->lang, std::string("de"));
        CHECK_EQ(d->title, std::string("Br\xC3\xB6tchen"));
    }
    auto q = parseWikipediaUrl("https://en.wikipedia.org/w/index.php?title=Sour_dough&action=view");
    CHECK(q.has_value() && q->title == "Sour dough");
    CHECK(!parseWikipediaUrl("https://example.com/wiki/X"));
    CHECK(!parseWikipediaUrl("https://en.wikipedia.org/"));
}

void testDetectInputKind() {
    CHECK(detectInputKind("https://example.com/post") == InputKind::Url);
    CHECK(detectInputKind("example.com/post") == InputKind::Url);
    CHECK(detectInputKind("  https://en.wikipedia.org/wiki/Bread \n") == InputKind::Wikipedia);
    CHECK(detectInputKind("<!DOCTYPE html><html><body><p>x</p></body></html>") == InputKind::Html);
    CHECK(detectInputKind("<p>Hello</p><p>World</p>") == InputKind::Html);
    CHECK(detectInputKind("<article>Only one tag</article>") == InputKind::Html);
    CHECK(detectInputKind("# Title\n\nSome text with a [link](https://x.y).") == InputKind::Markdown);
    CHECK(detectInputKind("Intro\n\n- one\n- two\n\n**bold** text") == InputKind::Markdown);
    CHECK(detectInputKind("```\ncode\n```") == InputKind::Markdown);
    CHECK(detectInputKind("Just a sentence. And another one.\n\nA second paragraph.") == InputKind::Text);
    CHECK(detectInputKind("") == InputKind::Text);
    CHECK(detectInputKind("a < b and c > d") == InputKind::Text);
    CHECK(detectInputKind("{\"v\":1,\"url\":\"https://x.y\",\"html\":\"<p>a</p>\",\"capturedAt\":\"2026-01-01\"}") == InputKind::Bookmarklet);
    CHECK(detectInputKind("# not markdown?", "notes.txt") == InputKind::Text);
    CHECK(detectInputKind("plain words", "README.md") == InputKind::Markdown);
    CHECK(detectInputKind("plain words", "page.HTM") == InputKind::Html);
    CHECK(detectInputKind("<p>a</p><p>b</p>", "unknown.bin") == InputKind::Html);
    CHECK(detectInputKind("| a | b |\n| --- | --- |\n| 1 | 2 |") == InputKind::Markdown);
}

// ------------------------------------------------------------- sanitize

void testSanitize() {
    const std::string dirty = R"HTML(
    <div class="wrap" style="color:red" onclick="evil()">
      <script>alert(1)</script><style>p{}</style>
      <nav><a href="/home">Home</a></nav>
      <header><p>Site header</p></header>
      <h2 class="t" data-x="1">Heading</h2>
      <p id="keep" title="drop" onmouseover="x()">Text <font color="red">old</font> <span style="x">span</span>
        <a href="/rel/page" target="_blank" rel="noopener">rel</a>
        <a href="javascript:alert(1)">js</a>
        <a href="data:text/html,<b>x</b>">data</a>
        <a href="#frag">frag</a>
      </p>
      <img src="pic.png" alt="A pic" width="10" height="10" onerror="x()">
      <img src="data:image/png;base64,iVBORw0KGgo=" alt="inline">
      <iframe src="https://evil.example"></iframe>
      <form><input value="x"></form>
      <table border="1"><tr><td colspan="2" rowspan="1" class="c">cell</td></tr><tr><td>a</td><td>b</td></tr></table>
      <video src="x.mp4"></video>
      <aside>Aside text</aside>
      <footer>Footer text</footer>
      <div role="navigation">Role nav</div>
    </div>)HTML";
    ExtractOptions o;
    std::string out = sanitizeHtml(dirty, o, "https://site.example/blog/post");
    CHECK(wellFormed(out));
    std::set<std::string> tags, attrs;
    tagsAndAttrs(out, tags, attrs);
    const std::set<std::string> allowedTags = {"h1", "h2", "h3", "h4", "h5", "h6", "p", "br", "hr", "ul", "ol", "li", "blockquote", "pre",
                                               "code", "b", "strong", "i", "em", "u", "s", "del", "sub", "sup", "span", "a", "img",
                                               "figure", "figcaption", "table", "thead", "tbody", "tr", "th", "td", "caption", "dl", "dt", "dd"};
    const std::set<std::string> allowedAttrs = {"href", "src", "alt", "id", "colspan", "rowspan"};
    for (const auto& t : tags) CHECK(allowedTags.count(t));
    for (const auto& a : attrs) CHECK(allowedAttrs.count(a));
    CHECK(!matches(out, "alert|<script|<style|<iframe|<form|<video"));
    CHECK(!has(out, "Site header"));
    CHECK(!has(out, "Aside text"));
    CHECK(!has(out, "Footer text"));
    CHECK(!has(out, "Role nav"));
    CHECK(!has(out, "Home"));
    CHECK(has(out, "href=\"https://site.example/rel/page\""));
    CHECK(has(out, "src=\"https://site.example/blog/pic.png\""));
    CHECK(has(out, "href=\"#frag\""));
    CHECK(!matches(out, "href=\"(javascript|data):"));
    CHECK(has(out, "js"));
    CHECK(has(out, "src=\"data:image/png;base64,iVBORw0KGgo=\""));
    CHECK(has(out, "colspan=\"2\""));
    CHECK(has(out, "id=\"keep\""));

    ExtractOptions noLinks;
    noLinks.keepLinks = false;
    noLinks.keepImages = false;
    std::string o2 = sanitizeHtml("<p>A <a href=\"https://x.y\">link</a> <img src=\"https://x.y/a.jpg\" alt=\"i\"></p>", noLinks, "");
    CHECK(!has(o2, "<a"));
    CHECK(!has(o2, "<img"));
    CHECK(has(o2, "link"));

    CHECK_EQ(sanitizeHtml("<div><div>One</div><section><div>Two</div><p>Three</p></section>Loose <b>four</b></div>", o, ""),
             std::string("<p>One</p><p>Two</p><p>Three</p><p>Loose <b>four</b></p>"));
    CHECK_EQ(sanitizeHtml("<figure class=\"x\"><div class=\"inner\"><picture><source srcset=\"/a.webp\" type=\"image/webp\"><img src=\"/a.jpg\" alt=\"A\"></picture></div><figcaption><p>Cap</p></figcaption></figure>",
                          o, "https://s.example/"),
             std::string("<figure><img src=\"https://s.example/a.jpg\" alt=\"A\"/><figcaption>Cap</figcaption></figure>"));
    ExtractOptions noTables;
    noTables.keepTables = false;
    CHECK_EQ(sanitizeHtml("<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>", noTables, ""),
             std::string("<p>A | B</p><p>1 | 2</p>"));
    std::string layout = sanitizeHtml("<table><tr><td><p>Layout</p><table><tr><td>a</td><td>b</td></tr></table></td></tr></table>", o, "");
    CHECK(has(layout, "<p>Layout</p>"));
    CHECK_EQ(countOf(layout, "<table>"), 1);
    CHECK_EQ(sanitizeHtml("<pre><code class=\"lang-js\">let a = 1;\n  a++;</code></pre>", o, ""), std::string("<pre><code>let a = 1;\n  a++;</code></pre>"));

    // Void tags self-close, text is escaped, empty elements are not self-closed.
    std::string v = sanitizeHtml("<p>a &amp; b &lt; c<br>d</p><hr><p>x<a id=\"n\"></a>y</p>", o, "");
    CHECK(wellFormed(v));
    CHECK(has(v, "<br/>"));
    CHECK(has(v, "<hr/>"));
    CHECK(has(v, "a &amp; b &lt; c"));
    CHECK(has(v, "x<span id=\"n\"></span>y"));
    // Control characters and bad UTF-8 do not break the XML.
    CHECK(wellFormed(sanitizeHtml(std::string("<p>bad \x01\x0c byte \xff end</p>"), o, "")));
    // A figcaption outside a figure becomes a paragraph.
    CHECK_EQ(sanitizeHtml("<figcaption>Lonely caption</figcaption>", o, ""), std::string("<p>Lonely caption</p>"));
}

// ------------------------------------------------------------- articles

void testBlog() {
    std::string html = fixture("article-blog.html");
    CHECK(!html.empty());
    RawInput in;
    in.kind = InputKind::Url;
    in.content = html;
    in.sourceUrl = "https://crumbjournal.example/posts/slow-bread";
    Article a = extractArticle(in);
    CHECK_EQ(a.title, std::string("Why Slow Bread Tastes Better"));
    CHECK_EQ(a.author, std::string("Ada Baker"));
    CHECK_EQ(a.publishedAt, std::string("2025-03-14T09:30:00.000Z"));
    CHECK_EQ(a.siteName, std::string("The Crumb Journal"));
    CHECK(has(a.excerpt, "long fermentation"));
    CHECK_EQ(a.language, std::string("en-GB"));
    CHECK(a.inputKind == InputKind::Url);
    CHECK_EQ(a.sourceUrl, std::string("https://crumbjournal.example/posts/slow-bread"));
    CHECK(matches(a.id, "^[0-9a-f-]{36}$"));
    CHECK(!a.capturedAt.empty());
    CHECK((a.tags == std::vector<std::string>{"baking", "fermentation"}));

    CHECK(has(a.bodyHtml, "Most home bakers rush the dough"));
    CHECK(has(a.bodyHtml, "Short rises do not give the enzymes"));
    CHECK(has(a.bodyHtml, "Time is the cheapest ingredient"));
    CHECK(matches(a.bodyHtml, "<pre><code>hydration"));
    CHECK(matches(a.bodyHtml, "<ol>[\\s\\S]*<li>Mix at 6 pm\\.</li>"));
    CHECK(matches(a.bodyHtml, "<table>[\\s\\S]*Cold proof"));
    CHECK(a.wordCount > 200);
    for (const char* junk : {"cookies", "ADVERTISEMENT", "Share on", "Related posts", "casino", "All rights reserved", "Subscribe", "Ten tips", "Recipes"})
        CHECK(!has(a.bodyHtml, junk));
    CHECK(!matches(a.bodyHtml, "<script|<style|<iframe|<form|onclick|style=|class="));
    CHECK(!has(a.bodyHtml, "javascript:"));
    // The title heading and the byline are not in the body.
    CHECK(!has(a.bodyHtml, "<h1"));
    CHECK(!has(a.bodyHtml, "14 March 2025"));

    CHECK(has(a.bodyHtml, "href=\"https://crumbjournal.example/glossary/long-fermentation\""));
    CHECK_EQ(a.images.size(), size_t(2));
    if (a.images.size() == 2) {
        CHECK_EQ(a.images[0].src, std::string("https://crumbjournal.example/images/crumb-480.jpg"));
        CHECK_EQ(a.images[0].alt, std::string("Open crumb of a slow-fermented loaf"));
        CHECK_EQ(a.images[1].src, std::string("https://cdn.crumbjournal.example/img/fridge.jpg"));
        CHECK_EQ(a.images[1].alt, std::string("Loaves in a fridge"));
    }
    CHECK(has(a.bodyHtml,
              "<figure><img src=\"https://crumbjournal.example/images/crumb-480.jpg\" alt=\"Open crumb of a slow-fermented loaf\"/><figcaption>An open crumb after an 18-hour cold rise.</figcaption></figure>"));
    CHECK(matches(a.bodyHtml, "<figure><img src=\"https://cdn\\.crumbjournal\\.example/img/fridge\\.jpg\"[^>]*/><figcaption>Shaped loaves"));

    std::vector<std::string> tocText, tocIds;
    for (const auto& t : a.toc) {
        tocText.push_back(t.text);
        tocIds.push_back(t.id);
    }
    CHECK((tocText == std::vector<std::string>{"What happens during a slow rise", "Using the fridge", "Timing table", "A simple schedule"}));
    CHECK((tocIds == std::vector<std::string>{"sec-1", "sec-2", "sec-3", "sec-4"}));
    CHECK(has(a.bodyHtml, "<h2 id=\"sec-1\">"));
    CHECK(has(a.bodyHtml, "<h3 id=\"sec-3\">"));
    CHECK(wellFormed(a.bodyHtml));

    RawInput in2;
    in2.kind = InputKind::Html;
    in2.content = html;
    ExtractOptions o;
    o.keepLinks = false;
    o.keepImages = false;
    Article b = extractArticle(in2, o);
    CHECK(!has(b.bodyHtml, "<a "));
    CHECK(!has(b.bodyHtml, "<img"));
    CHECK(b.images.empty());
    CHECK(b.inputKind == InputKind::Html);
    CHECK_EQ(b.title, std::string("Why Slow Bread Tastes Better"));
    CHECK(wellFormed(b.bodyHtml));
}

void testWikipedia() {
    std::string html = fixture("wikipedia-sample.html");
    CHECK(!html.empty());
    const std::string url = "https://en.wikipedia.org/wiki/Sourdough";
    Article a = extractWikipedia(html, url);
    CHECK_EQ(a.title, std::string("Sourdough"));
    CHECK_EQ(a.author, std::string("Wikipedia contributors"));
    CHECK_EQ(a.siteName, std::string("Wikipedia"));
    CHECK_EQ(a.language, std::string("en"));
    CHECK(a.inputKind == InputKind::Wikipedia);
    CHECK(has(a.bodyHtml, "is a type of"));
    CHECK(has(a.bodyHtml, "href=\"https://en.wikipedia.org/wiki/Bread\""));
    for (const char* gone : {"InfoboxTypeBread", "ReferenceOneText", "SeeAlsoRyeBread", "ExternalLinkSite", "NavboxBreads", "Contents", "Categories",
                             "Jump to content", "needs additional citations", "For the gold rush miners", "Bread made by fermentation", "From Wikipedia",
                             "last edited", "[1]", "[edit]", ">edit<"})
        CHECK(!has(a.bodyHtml, gone));
    std::vector<std::string> toc;
    for (const auto& t : a.toc) toc.push_back(t.text);
    CHECK((toc == std::vector<std::string>{"History", "Gold rush", "Chemistry"}));
    CHECK(has(a.bodyHtml,
              "<figure><img src=\"https://upload.wikimedia.org/wikipedia/commons/thumb/b/b2/Boulangerie.jpg/440px-Boulangerie.jpg\" alt=\"A bakery display\"/><figcaption>Sourdough loaves in a French <a href=\"https://en.wikipedia.org/wiki/Boulangerie\">boulangerie</a></figcaption></figure>"));
    CHECK(has(a.bodyHtml, "<figcaption>A prospector in 1898</figcaption>"));
    CHECK(!has(a.bodyHtml, "srcset"));
    CHECK_EQ(a.images.size(), size_t(2));
    if (a.images.size() == 2) {
        CHECK_EQ(a.images[0].src, std::string("https://upload.wikimedia.org/wikipedia/commons/thumb/b/b2/Boulangerie.jpg/440px-Boulangerie.jpg"));
        CHECK_EQ(a.images[1].src, std::string("https://upload.wikimedia.org/wikipedia/commons/thumb/c/c3/Miner.jpg/220px-Miner.jpg"));
    }
    CHECK(matches(a.bodyHtml, "<table><caption>Typical starter composition</caption>"));
    CHECK(has(a.excerpt, "Sourdough is a type of bread"));
    CHECK(wellFormed(a.bodyHtml));

    ExtractOptions keep;
    keep.keepInfobox = true;
    keep.keepReferences = true;
    Article k = extractWikipedia(html, url, keep);
    CHECK(has(k.bodyHtml, "InfoboxTypeBread"));
    CHECK(has(k.bodyHtml, "ReferenceOneText"));
    bool refs = false;
    for (const auto& t : k.toc) refs = refs || t.text == "References";
    CHECK(refs);
    CHECK(!has(k.bodyHtml, "SeeAlsoRyeBread"));
    CHECK(!has(k.bodyHtml, "ExternalLinkSite"));
    CHECK(wellFormed(k.bodyHtml));

    RawInput in;
    in.kind = InputKind::Wikipedia;
    in.content = html;
    in.sourceUrl = url;
    Article w = extractArticle(in);
    CHECK_EQ(w.title, std::string("Sourdough"));
    CHECK(w.inputKind == InputKind::Wikipedia);
    // A url input with a Wikipedia sourceUrl uses the Wikipedia path.
    in.kind = InputKind::Url;
    Article u = extractArticle(in);
    CHECK_EQ(u.author, std::string("Wikipedia contributors"));
    CHECK(u.inputKind == InputKind::Url);
}

void testTextAndMarkdown() {
    RawInput md;
    md.kind = InputKind::Markdown;
    md.content = fixture("sample.md");
    md.fileName = "sample.md";
    Article a = extractArticle(md);
    CHECK_EQ(a.title, std::string("Field Notes on E-Ink Reading"));
    CHECK(has(a.bodyHtml, "<strong>easy on the eyes</strong>"));
    CHECK(has(a.bodyHtml, "<pre><code>"));
    CHECK(!has(a.bodyHtml, "script"));
    CHECK(!a.toc.empty());
    if (!a.toc.empty()) {
        CHECK_EQ(a.toc[0].id, std::string("sec-1"));
        CHECK_EQ(a.toc[0].level, 1);
        CHECK_EQ(a.toc[0].text, std::string("Field Notes on E-Ink Reading"));
    }
    CHECK(wellFormed(a.bodyHtml));

    RawInput txt;
    txt.kind = InputKind::Text;
    txt.content = fixture("sample.txt");
    txt.fileName = "paper.txt";
    Article t = extractArticle(txt);
    CHECK_EQ(t.title, std::string("A Short Note on Paper"));
    CHECK(has(t.bodyHtml, "&lt;b&gt;tags&lt;/b&gt;"));
    CHECK(has(t.bodyHtml, "Roses are red,<br/>violets are blue,"));
    CHECK(has(t.bodyHtml, "trade routes to the Middle East"));
    CHECK(t.wordCount > 40);
    CHECK(wellFormed(t.bodyHtml));

    RawInput empty;
    empty.kind = InputKind::Text;
    empty.fileName = "my_notes.txt";
    CHECK_EQ(extractArticle(empty).title, std::string("my notes"));

    std::string h = markdownToHtml("# T\n\nHello **world**\n\n<script>alert(1)</script>");
    CHECK(has(h, "<h1>T</h1>"));
    CHECK(has(h, "<strong>world</strong>"));
    CHECK(!has(h, "script"));
    CHECK(wellFormed(markdownToHtml("a  \nb\n\n---\n\n| x | y |\n|---|---|\n| 1 | 2 |\n\n![pic](https://x.y/p.png)")));

    CHECK_EQ(textToHtml("a <b> & c\n\nsecond"), std::string("<p>a &lt;b&gt; &amp; c</p>\n<p>second</p>"));
    CHECK_EQ(textToHtml("short\nlines"), std::string("<p>short<br/>lines</p>"));
    CHECK(has(textToHtml("This line is long enough to count as prose for the joiner.\nAnd this second line is also long enough to join."), "joiner. And"));

    CHECK_EQ(countWords("<p>one two</p><p>three</p><ul><li>four</li><li>five six</li></ul>"), 6);
    CHECK_EQ(countWords(""), 0);

    std::string plain = htmlToText("<h2>Head</h2><p>One <b>two</b></p><ul><li>a</li><li>b</li></ul><p>x<br/>y</p>");
    CHECK_EQ(plain, std::string("Head\n\nOne two\n\na\nb\n\nx\ny"));
}

void testBookmarkletAndErrors() {
    std::string html = fixture("article-blog.html");
    // Escape the HTML as a JSON string.
    std::string esc;
    for (char c : html) {
        switch (c) {
            case '"': esc += "\\\""; break;
            case '\\': esc += "\\\\"; break;
            case '\n': esc += "\\n"; break;
            case '\r': esc += "\\r"; break;
            case '\t': esc += "\\t"; break;
            default: esc += c;
        }
    }
    RawInput in;
    in.kind = InputKind::Bookmarklet;
    in.content = "{\"v\":1,\"url\":\"https://crumbjournal.example/posts/slow-bread\",\"title\":\"Captured title\",\"html\":\"" + esc +
                 "\",\"capturedAt\":\"2026-10-01T12:00:00.000Z\"}";
    Article a = extractArticle(in);
    CHECK(a.inputKind == InputKind::Bookmarklet);
    CHECK_EQ(a.capturedAt, std::string("2026-10-01T12:00:00.000Z"));
    CHECK_EQ(a.sourceUrl, std::string("https://crumbjournal.example/posts/slow-bread"));
    CHECK(has(a.bodyHtml, "Most home bakers rush the dough"));

    bool threw = false;
    try {
        RawInput e;
        e.kind = InputKind::Url;
        extractArticle(e);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        RawInput e;
        e.kind = InputKind::Bookmarklet;
        e.content = "{\"v\":2,\"html\":\"<p>x</p>\"}";
        extractArticle(e);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testFetchImagesDataUrl() {
    Article a;
    a.images.push_back(ArticleImage{"data:image/png;base64,iVBORw0KGgo=", ""});
    a.images.push_back(ArticleImage{"data:text/plain,hello%20world", ""});
    ImageMap m = fetchImages(a);
    CHECK_EQ(m.size(), size_t(2));
    CHECK_EQ(m["data:image/png;base64,iVBORw0KGgo="], std::string("\x89PNG\r\n\x1a\n"));
    CHECK_EQ(m["data:text/plain,hello%20world"], std::string("hello world"));
    // A bad URL fails without a throw.
    FetchResult r = fetchUrl("http://127.0.0.1:1/", nullptr, 3);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

} // namespace

int main() {
    testNormalizeUrl();
    testWikipediaUrl();
    testDetectInputKind();
    testSanitize();
    testBlog();
    testWikipedia();
    testTextAndMarkdown();
    testBookmarkletAndErrors();
    testFetchImagesDataUrl();
    TEST_MAIN_END();
}
