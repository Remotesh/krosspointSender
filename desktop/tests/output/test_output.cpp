// Tests for the output module (Agent F).
#include "check.h"
#include "output/output.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <miniz.h>

#include "stb_image.h"
#include "stb_image_write.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace ks;
using namespace ks::output;

namespace {

bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

void appendBytes(void* ctx, void* data, int size) {
    static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size));
}

// A 900x600 RGB gradient as PNG bytes.
std::string makePng(int w, int h) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &px[(static_cast<size_t>(y) * w + x) * 3];
            p[0] = static_cast<unsigned char>(x * 255 / w);
            p[1] = static_cast<unsigned char>(y * 255 / h);
            p[2] = 128;
        }
    std::string out;
    stbi_write_png_to_func(appendBytes, &out, w, h, 3, px.data(), w * 3);
    return out;
}

struct ZipEntryInfo {
    std::string name;
    std::string data;
    int method = -1;
};

std::vector<ZipEntryInfo> readZip(const std::string& bytes) {
    std::vector<ZipEntryInfo> out;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) return out;
    const mz_uint n = mz_zip_reader_get_num_files(&z);
    for (mz_uint i = 0; i < n; ++i) {
        mz_zip_archive_file_stat st{};
        mz_zip_reader_file_stat(&z, i, &st);
        ZipEntryInfo e;
        e.name = st.m_filename;
        e.method = st.m_method;
        size_t size = 0;
        void* p = mz_zip_reader_extract_to_heap(&z, i, &size, 0);
        if (p) {
            e.data.assign(static_cast<const char*>(p), size);
            mz_free(p);
        }
        out.push_back(e);
    }
    mz_zip_reader_end(&z);
    return out;
}

// Parses with the strict XML parser. True when the document is well-formed.
bool xmlWellFormed(const std::string& s) {
    xmlParserCtxtPtr ctxt = xmlNewParserCtxt();
    xmlDocPtr d = xmlCtxtReadMemory(ctxt, s.data(), static_cast<int>(s.size()), "x.xml", "UTF-8", XML_PARSE_NONET);
    bool ok = d && ctxt->wellFormed && ctxt->nsWellFormed;
    if (d) xmlFreeDoc(d);
    xmlFreeParserCtxt(ctxt);
    return ok;
}

void collectNavPoints(xmlNodePtr n, std::vector<std::pair<std::string, int>>& out) {
    for (xmlNodePtr c = n; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char*>(c->name), "navPoint") == 0) {
            xmlChar* id = xmlGetProp(c, BAD_CAST "id");
            xmlChar* po = xmlGetProp(c, BAD_CAST "playOrder");
            out.push_back({id ? reinterpret_cast<const char*>(id) : "", po ? std::atoi(reinterpret_cast<const char*>(po)) : -1});
            if (id) xmlFree(id);
            if (po) xmlFree(po);
        }
        if (c->children) collectNavPoints(c->children, out);
    }
}

Article sampleArticle() {
    Article a;
    a.id = "0b6b1c1e-3f7d-4a8e-9c2b-1f2e3d4c5b6a";
    a.title = "Sample: A \"Test\" Article";
    a.author = "Ada Lovelace";
    a.publishedAt = "2026-01-02T10:00:00Z";
    a.capturedAt = "2026-10-10T16:00:00Z";
    a.sourceUrl = "https://example.com/post?a=1&b=2";
    a.siteName = "Example & Co";
    a.excerpt = "An excerpt.";
    a.language = "en";
    a.tags = {"test", "demo"};
    a.wordCount = 460;
    a.inputKind = InputKind::Url;
    a.bodyHtml =
        "<p>Intro paragraph with &amp; and \xC3\xA9 and&nbsp;nbsp.</p>"
        "<h2 id=\"one\">First section</h2>"
        "<p>See <a href=\"#two\">the second part</a> and <a href=\"https://example.com/x?a=1&amp;b=2\">a link</a>.</p>"
        "<figure><img src=\"https://example.com/a.png\" alt=\"Gradient\"/><figcaption>A caption</figcaption></figure>"
        "<p><img src=\"https://example.com/missing.webp\" alt=\"Missing image\"/></p>"
        "<ul><li>Apple</li><li>Pear</li></ul>"
        "<ol><li>One</li><li>Two</li></ol>"
        "<h3>Sub heading</h3>"
        "<pre><code>int x = 1 &lt; 2;</code></pre>"
        "<h2 id=\"two\">Second section</h2>"
        "<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>"
        "<blockquote><p>Quoted text.</p></blockquote>"
        "<p>End.<br/>Line two.</p>";
    a.images = {{"https://example.com/a.png", "Gradient"}, {"https://example.com/missing.webp", "Missing image"}};
    return a;
}

void testFilenames() {
    CHECK_EQ(sanitizeFilename("a<b>c"), std::string("a b c"));
    CHECK_EQ(sanitizeFilename("  ..hello world.. "), std::string("hello world"));
    CHECK_EQ(sanitizeFilename("CON"), std::string("_CON"));
    CHECK_EQ(sanitizeFilename("lpt1"), std::string("_lpt1"));
    CHECK_EQ(sanitizeFilename(""), std::string("untitled"));
    CHECK_EQ(sanitizeFilename("///"), std::string("untitled"));
    CHECK_EQ(sanitizeFilename("a\tb\n\nc"), std::string("a b c"));
    FilenameRules r;
    r.spaceReplacement = "-";
    CHECK_EQ(sanitizeFilename("a - b c", r), std::string("a-b-c"));
    r.lowercase = true;
    CHECK_EQ(sanitizeFilename("Hello World", r), std::string("hello-world"));
    FilenameRules shortRule;
    shortRule.maxLength = 5;
    CHECK_EQ(sanitizeFilename("abcdefgh", shortRule), std::string("abcde"));
    CHECK_EQ(sanitizeFilename("abcd efgh", shortRule), std::string("abcd"));
    // Code points, not bytes: 3 x "é" fit into maxLength 3.
    FilenameRules three;
    three.maxLength = 3;
    CHECK_EQ(sanitizeFilename("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", three), std::string("\xC3\xA9\xC3\xA9\xC3\xA9"));

    Article a;
    a.title = "My Title";
    a.publishedAt = "2026-01-02";
    a.capturedAt = "2026-10-10T00:00:00Z";
    FilenameRules t;
    CHECK_EQ(makeFilename(a, OutputFormat::Epub, t), std::string("My Title.epub"));
    t.templ = "{title} - {author}";
    CHECK_EQ(makeFilename(a, OutputFormat::Epub, t), std::string("My Title.epub"));
    t.templ = "{date} {title}";
    CHECK_EQ(makeFilename(a, OutputFormat::Md, t), std::string("2026-01-02 My Title.md"));
    t.templ = "[{site}] {title}";
    CHECK_EQ(makeFilename(a, OutputFormat::Txt, t), std::string("My Title.txt"));
    t.templ = "{author} - {site} - {title}";
    CHECK_EQ(makeFilename(a, OutputFormat::Html, t), std::string("My Title.html"));
    a.author = "Ann";
    t.templ = "{author} - {site} - {title}";
    CHECK_EQ(makeFilename(a, OutputFormat::Html, t), std::string("Ann - My Title.html"));
    t.templ = "{year}_{title}";
    CHECK_EQ(makeFilename(a, OutputFormat::Zip, t), std::string("2026_My Title.zip"));
    t.templ = "";
    a.title = "";
    CHECK_EQ(makeFilename(a, OutputFormat::Epub, t), std::string("untitled.epub"));
}

void testPresetsCss() {
    CHECK_EQ(preset(StylePreset::Compact).fontSizePercent, 90);
    CHECK_EQ(preset(StylePreset::Large).fontSizePercent, 125);
    CHECK(preset(StylePreset::Compact).tableHandling == TableHandling::Linearize);
    std::string css = epubCss(preset(StylePreset::Comfortable));
    CHECK(contains(css, "/* K-rosspointSender stylesheet. Preset: comfortable. */"));
    CHECK(contains(css, "margin: 0.8em;"));
    CHECK(contains(css, "font-size: 100%;"));
    CHECK(contains(css, "line-height: 1.45;"));
    CHECK(contains(css, "hyphens: manual;"));
    StyleSettings s = preset(StylePreset::Compact);
    s.customCss = "  p { color: black; }  ";
    css = epubCss(s);
    CHECK(contains(css, "text-align: justify;"));
    CHECK(contains(css, "\n/* Custom CSS */\np { color: black; }\n"));
}

void testProcessImage() {
    std::string png = makePng(900, 600);
    CHECK(!png.empty());
    auto p = processImage(png, 480, true, 85);
    CHECK(p.has_value());
    if (!p) return;
    CHECK_EQ(p->width, 480);
    CHECK_EQ(p->height, 320);
    CHECK_EQ(p->mime, std::string("image/jpeg"));
    CHECK(p->bytes.size() > 3);
    CHECK(static_cast<unsigned char>(p->bytes[0]) == 0xFF && static_cast<unsigned char>(p->bytes[1]) == 0xD8 &&
          static_cast<unsigned char>(p->bytes[2]) == 0xFF);
    int w = 0, h = 0, comp = 0;
    CHECK(stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(p->bytes.data()), static_cast<int>(p->bytes.size()),
                                &w, &h, &comp));
    CHECK_EQ(w, 480);
    // stb writes YCbCr JPEG even for one input channel. Check that the pixels are gray.
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(p->bytes.data()),
                                        static_cast<int>(p->bytes.size()), &w, &h, &comp, 3);
    CHECK(px != nullptr);
    if (px) {
        int maxDiff = 0;
        for (int i = 0; i < w * h; ++i) {
            maxDiff = std::max(maxDiff, std::abs(px[i * 3] - px[i * 3 + 1]));
            maxDiff = std::max(maxDiff, std::abs(px[i * 3 + 1] - px[i * 3 + 2]));
        }
        CHECK(maxDiff <= 2);
        stbi_image_free(px);
    }

    auto colour = processImage(png, 2000, false, 80);
    CHECK(colour.has_value());
    if (colour) {
        CHECK_EQ(colour->width, 900);  // never scales up
        stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(colour->bytes.data()),
                              static_cast<int>(colour->bytes.size()), &w, &h, &comp);
        CHECK_EQ(comp, 3);
    }
    CHECK(!processImage("RIFF\0\0\0\0WEBPVP8 garbage", 480, true, 85).has_value());
    CHECK(!processImage("<svg xmlns='http://www.w3.org/2000/svg'/>", 480, true, 85).has_value());
    CHECK(!processImage("", 480, true, 85).has_value());
}

void testEstimate() {
    Article a;
    a.wordCount = 460;
    Estimate e = estimateReading(a, preset(StylePreset::Comfortable));
    CHECK_EQ(e.minutes, 2);
    CHECK_EQ(e.pages, 3);
    e = estimateReading(a, preset(StylePreset::Large));  // 120 * 100 / 125 = 96 words per page
    CHECK_EQ(e.pages, 5);
    e = estimateReading(a, preset(StylePreset::Compact));  // 210 * 100 / 90 = 233.3
    CHECK_EQ(e.pages, 2);
    Article empty;
    e = estimateReading(empty, preset(StylePreset::Comfortable));
    CHECK_EQ(e.minutes, 0);
    CHECK_EQ(e.pages, 1);
    Article counted;
    counted.bodyHtml = "<p>one two three</p><p>four</p>";
    e = estimateReading(counted, preset(StylePreset::Comfortable));
    CHECK_EQ(e.minutes, 1);
    CHECK_EQ(e.pages, 1);
}

// Checks the EPUB rules. Returns the entries for more checks.
std::map<std::string, std::string> checkEpub(const std::string& bytes, const char* label) {
    std::map<std::string, std::string> files;
    // mimetype: first local header, stored, no extra field.
    CHECK(bytes.size() > 58);
    CHECK(bytes.compare(0, 4, "PK\x03\x04") == 0);
    CHECK(bytes.compare(30, 8, "mimetype") == 0);
    CHECK(bytes[28] == 0 && bytes[29] == 0);  // extra field length 0
    CHECK(bytes.compare(38, 20, "application/epub+zip") == 0);

    auto entries = readZip(bytes);
    CHECK(!entries.empty());
    if (entries.empty()) return files;
    CHECK_EQ(entries[0].name, std::string("mimetype"));
    CHECK_EQ(entries[0].method, 0);
    CHECK_EQ(entries[0].data, std::string("application/epub+zip"));
    for (const auto& e : entries) {
        CHECK(!e.name.empty() && e.name.back() != '/');  // no directory entries
        files[e.name] = e.data;
    }
    CHECK(files.count("META-INF/container.xml"));
    CHECK(files.count("OEBPS/content.opf"));
    CHECK(files.count("OEBPS/nav.xhtml"));
    CHECK(files.count("OEBPS/toc.ncx"));
    CHECK(files.count("OEBPS/style.css"));
    for (const auto& [name, data] : files) {
        bool xml = name.size() > 6 && (name.compare(name.size() - 6, 6, ".xhtml") == 0 ||
                                       name.compare(name.size() - 4, 4, ".opf") == 0 ||
                                       name.compare(name.size() - 4, 4, ".ncx") == 0 ||
                                       name.compare(name.size() - 4, 4, ".xml") == 0);
        if (xml) {
            bool ok = xmlWellFormed(data);
            if (!ok) std::fprintf(stderr, "[%s] not well-formed: %s\n", label, name.c_str());
            CHECK(ok);
            CHECK(!contains(data, "src=\"http"));
            CHECK(!contains(data, "src='http"));
        }
    }
    // NCX: playOrder 1..N in document order, unique ids.
    xmlDocPtr ncx = xmlReadMemory(files["OEBPS/toc.ncx"].data(), static_cast<int>(files["OEBPS/toc.ncx"].size()),
                                  "toc.ncx", "UTF-8", XML_PARSE_NONET);
    CHECK(ncx != nullptr);
    if (ncx) {
        std::vector<std::pair<std::string, int>> points;
        collectNavPoints(xmlDocGetRootElement(ncx), points);
        CHECK(!points.empty());
        std::set<std::string> ids;
        for (size_t i = 0; i < points.size(); ++i) {
            CHECK_EQ(points[i].second, static_cast<int>(i + 1));
            CHECK(ids.insert(points[i].first).second);
        }
        xmlFreeDoc(ncx);
    }
    return files;
}

void testEpubBuild() {
    Article a = sampleArticle();
    BuildOptions o;
    o.style = preset(StylePreset::Comfortable);
    ImageMap images;
    images["https://example.com/a.png"] = makePng(900, 600);
    images["https://example.com/missing.webp"] = "RIFF\0\0\0\0WEBP";  // not decodable
    OutputFile f = buildEpub(a, o, images);
    CHECK_EQ(f.name, std::string("Sample A Test Article.epub"));
    CHECK_EQ(f.mime, std::string("application/epub+zip"));
    CHECK(f.format == OutputFormat::Epub);
    CHECK_EQ(f.estimatedPages, 3);
    writeFile(std::string(KS_TEST_OUT_DIR) + "/sample.epub", f.bytes);

    auto files = checkEpub(f.bytes, "sample");
    const std::string& opf = files["OEBPS/content.opf"];
    CHECK(contains(opf, "<dc:title>Sample: A &quot;Test&quot; Article</dc:title>"));
    CHECK(contains(opf, "<dc:source>https://example.com/post?a=1&amp;b=2</dc:source>"));
    CHECK(contains(opf, "<dc:identifier id=\"bookid\">urn:uuid:0b6b1c1e-3f7d-4a8e-9c2b-1f2e3d4c5b6a</dc:identifier>"));
    CHECK(contains(opf, "<dc:date>2026-01-02</dc:date>"));
    CHECK(contains(opf, "<dc:publisher>Example &amp; Co</dc:publisher>"));
    CHECK(contains(opf, "href=\"images/img-001.jpg\" media-type=\"image/jpeg\""));
    CHECK(contains(opf, "<itemref idref=\"cover\"/>"));
    CHECK(files.count("OEBPS/cover.xhtml"));
    CHECK(files.count("OEBPS/images/img-001.jpg"));
    CHECK(!files.count("OEBPS/images/img-002.jpg"));
    // Preface + two h2 sections = three chapters.
    CHECK(files.count("OEBPS/chapter-001.xhtml"));
    CHECK(files.count("OEBPS/chapter-002.xhtml"));
    CHECK(files.count("OEBPS/chapter-003.xhtml"));
    CHECK(!files.count("OEBPS/chapter-004.xhtml"));
    const std::string& c1 = files["OEBPS/chapter-001.xhtml"];
    CHECK(contains(c1, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"en\" xml:lang=\"en\">"));
    CHECK(contains(c1, "\xC3\xA9"));  // UTF-8 kept, not a character reference
    CHECK(contains(c1, "\xC2\xA0"));  // &nbsp; became U+00A0
    const std::string& c2 = files["OEBPS/chapter-002.xhtml"];
    CHECK(contains(c2, "<img src=\"images/img-001.jpg\" alt=\"Gradient\"/>"));
    CHECK(contains(c2, "<p><span class=\"img-alt\">Missing image</span></p>"));  // inside <p>: a span
    CHECK(contains(c2, "href=\"chapter-003.xhtml#two\""));
    CHECK(contains(c2, "href=\"https://example.com/x?a=1&amp;b=2\""));
    CHECK(contains(c2, "<p class=\"caption\">A caption</p>"));
    CHECK(contains(c2, "id=\"sec-1\""));  // h3 without id
    const std::string& c3 = files["OEBPS/chapter-003.xhtml"];
    CHECK(contains(c3, "<table>"));
    CHECK(contains(c3, "<section class=\"source-note\"><hr/><p>Source: https://example.com/post?a=1&amp;b=2, captured 2026-10-10, generated by K-rosspointSender.</p></section>"));
    const std::string& cover = files["OEBPS/cover.xhtml"];
    CHECK(contains(cover, "epub:type=\"titlepage\""));
    CHECK(contains(cover, "<b>Words: </b>460"));
    const std::string& nav = files["OEBPS/nav.xhtml"];
    CHECK(contains(nav, "<nav epub:type=\"toc\" id=\"toc\">"));
    CHECK(contains(nav, "<a href=\"cover.xhtml\">Title page</a>"));
    CHECK(contains(nav, "chapter-002.xhtml#sec-1"));

    // No images: every img becomes alt text. Linearized tables. No cover.
    BuildOptions o2;
    o2.style = preset(StylePreset::Compact);
    o2.includeImages = false;
    o2.includeCover = false;
    OutputFile f2 = buildEpub(a, o2, images);
    auto files2 = checkEpub(f2.bytes, "compact");
    CHECK(!files2.count("OEBPS/cover.xhtml"));
    CHECK(!files2.count("OEBPS/images/img-001.jpg"));
    CHECK(contains(files2["OEBPS/chapter-001.xhtml"], "<h1 class=\"title\">"));
    CHECK(contains(files2["OEBPS/chapter-001.xhtml"], "<p class=\"byline\">Ada Lovelace \xC2\xB7 Example &amp; Co \xC2\xB7 2026-01-02</p>"));
    CHECK(contains(files2["OEBPS/chapter-002.xhtml"], "<p class=\"img-alt\">Gradient</p>"));
    CHECK(contains(files2["OEBPS/chapter-003.xhtml"], "class=\"table-linear\""));
    CHECK(!contains(files2["OEBPS/chapter-002.xhtml"], "https://example.com/x"));  // keepLinks off

    // A body that is not well-formed XML uses the HTML parser.
    Article bad;
    bad.title = "Broken";
    bad.bodyHtml = "<p>Open <b>bold<p>Next &copy; &unknown; <br>end";
    OutputFile f3 = buildEpub(bad, o);
    auto files3 = checkEpub(f3.bytes, "broken");
    CHECK(contains(files3["OEBPS/chapter-001.xhtml"], "Next"));
    CHECK(files3.count("OEBPS/chapter-001.xhtml") && !files3.count("OEBPS/chapter-002.xhtml"));
}

void testBundle() {
    Article a = sampleArticle();
    Article b;
    b.id = "x";
    b.title = "Second";
    b.bodyHtml = "<h2>Part</h2><p>Text</p><h2>More</h2><p>Text</p>";
    b.sourceUrl = "https://example.org/b";
    BuildOptions o;
    OutputFile f = buildBundleEpub({a, b}, "My Bundle", "Me", o, {{"https://example.com/a.png", makePng(100, 50)}});
    CHECK_EQ(f.name, std::string("My Bundle.epub"));
    auto files = checkEpub(f.bytes, "bundle");
    CHECK(files.count("OEBPS/chapter-001.xhtml"));
    CHECK(files.count("OEBPS/chapter-002.xhtml"));
    CHECK(!files.count("OEBPS/chapter-003.xhtml"));
    CHECK(files.count("OEBPS/images/img-a01-001.jpg"));
    CHECK(contains(files["OEBPS/cover.xhtml"], "<li>Second</li>"));
    CHECK(contains(files["OEBPS/content.opf"], "<dc:source>https://example.org/b</dc:source>"));
    writeFile(std::string(KS_TEST_OUT_DIR) + "/sample-bundle.epub", f.bytes);

    OutputFile empty = buildBundleEpub({}, "", "", o);
    auto files2 = checkEpub(empty.bytes, "empty-bundle");
    CHECK_EQ(empty.name, std::string("Articles.epub"));
    CHECK(contains(files2["OEBPS/chapter-001.xhtml"], "This bundle has no articles."));
}

void testTestEpub() {
    OutputFile t = testEpub();
    CHECK_EQ(t.name, std::string("krosspoint-test.epub"));
    auto files = checkEpub(t.bytes, "test");
    CHECK(contains(files["OEBPS/content.opf"], "<dc:title>K-rosspointSender test</dc:title>"));
    CHECK(contains(files["OEBPS/chapter-001.xhtml"], "The transfer works."));
    CHECK(!files.count("OEBPS/cover.xhtml"));
    writeFile(std::string(KS_TEST_OUT_DIR) + "/krosspoint-test.epub", t.bytes);
}

void testTextFormats() {
    Article a = sampleArticle();
    BuildOptions o;
    o.style = preset(StylePreset::Comfortable);
    OutputFile txt = buildTxt(a, o);
    CHECK_EQ(txt.name, std::string("Sample A Test Article.txt"));
    const std::string& t = txt.bytes;
    CHECK(contains(t, "Sample: A \"Test\" Article\n========================"));
    CHECK(contains(t, "Author: Ada Lovelace"));
    CHECK(contains(t, "First section\n-------------"));
    CHECK(contains(t, "- Apple\n- Pear"));
    CHECK(contains(t, "1. One\n2. Two"));
    CHECK(contains(t, "[Image: Gradient]"));
    CHECK(contains(t, "a link (https://example.com/x?a=1&b=2)"));
    CHECK(contains(t, "    int x = 1 < 2;"));
    CHECK(contains(t, "A | B\n1 | 2"));
    CHECK(contains(t, "> Quoted text."));
    CHECK(contains(t, "End.\nLine two."));
    CHECK(contains(t, "* * *\n\nSource: https://example.com/post?a=1&b=2, captured 2026-10-10, generated by K-rosspointSender.\n"));

    OutputFile md = buildMarkdown(a, o);
    CHECK_EQ(md.name, std::string("Sample A Test Article.md"));
    const std::string& m = md.bytes;
    CHECK(m.rfind("# Sample: A \"Test\" Article\n\n**Author:** Ada Lovelace  \n", 0) == 0);
    CHECK(contains(m, "## First section"));
    CHECK(contains(m, "### Sub heading"));
    CHECK(contains(m, "- Apple\n- Pear"));
    CHECK(contains(m, "1. One\n2. Two"));
    CHECK(contains(m, "```\nint x = 1 < 2;\n```"));
    CHECK(contains(m, "[a link](https://example.com/x?a=1&b=2)"));
    CHECK(contains(m, "![Gradient](https://example.com/a.png)"));
    CHECK(contains(m, "| A | B |\n| --- | --- |\n| 1 | 2 |"));
    CHECK(contains(m, "> Quoted text."));
    CHECK(contains(m, "End.  \nLine two."));
    CHECK(contains(m, "---\n\n*Source: "));

    OutputFile html = buildHtml(a, o);
    CHECK_EQ(html.name, std::string("Sample A Test Article.html"));
    const std::string& h = html.bytes;
    CHECK(h.rfind("<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">", 0) == 0);
    CHECK(contains(h, "<header class=\"cover\"><h1 class=\"title\">"));
    CHECK(contains(h, "<img src=\"https://example.com/a.png\" alt=\"Gradient\">"));
    CHECK(contains(h, "@media screen and (min-width: 40em)"));
    CHECK(contains(h, "<br>"));
    CHECK(!contains(h, "<br/>"));
    CHECK(!contains(h, "/>"));

    OutputFile zip = buildZip(a, o);
    CHECK_EQ(zip.name, std::string("Sample A Test Article.zip"));
    auto entries = readZip(zip.bytes);
    CHECK_EQ(entries.size(), static_cast<size_t>(5));
    std::set<std::string> names;
    for (auto& e : entries) names.insert(e.name);
    CHECK(names.count("Sample A Test Article.epub"));
    CHECK(names.count("Sample A Test Article.html"));
    CHECK(names.count("Sample A Test Article.txt"));
    CHECK(names.count("Sample A Test Article.md"));
    CHECK(names.count("article.json"));
    for (auto& e : entries)
        if (e.name == "article.json") {
            CHECK(contains(e.data, "\"title\": \"Sample: A \\\"Test\\\" Article\""));
            CHECK(contains(e.data, "\"inputKind\": \"url\""));
        }
}

} // namespace

int main() {
    xmlInitParser();
    testFilenames();
    testPresetsCss();
    testProcessImage();
    testEstimate();
    testEpubBuild();
    testBundle();
    testTestEpub();
    testTextFormats();
    TEST_MAIN_END();
}
