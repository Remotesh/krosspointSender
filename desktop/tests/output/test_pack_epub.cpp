// Tests for buildPackEpub and the definition-list fix (port of tests/epub/pack.test.ts
// and tests/epub/dl.test.ts of the web app). Writes build/pack-sample.epub.
#include "check.h"
#include "output/output.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <miniz.h>

#include "stb_image_write.h"

#include <algorithm>
#include <random>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace ks;
using namespace ks::output;

namespace {

bool has(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

struct Entry {
    std::string name, data;
    int method = -1;
};

std::vector<Entry> readZip(const std::string& bytes) {
    std::vector<Entry> out;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, bytes.data(), bytes.size(), 0)) return out;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
        mz_zip_archive_file_stat st{};
        mz_zip_reader_file_stat(&z, i, &st);
        Entry e{st.m_filename, "", st.m_method};
        size_t size = 0;
        if (void* p = mz_zip_reader_extract_to_heap(&z, i, &size, 0)) {
            e.data.assign(static_cast<const char*>(p), size);
            mz_free(p);
        }
        out.push_back(e);
    }
    mz_zip_reader_end(&z);
    return out;
}

const Entry* find(const std::vector<Entry>& z, const std::string& name) {
    for (const auto& e : z)
        if (e.name == name) return &e;
    return nullptr;
}

std::string text(const std::vector<Entry>& z, const std::string& name) {
    const Entry* e = find(z, name);
    if (!e) std::fprintf(stderr, "missing %s\n", name.c_str());
    return e ? e->data : "";
}

bool wellFormed(const std::string& s) {
    xmlParserCtxtPtr c = xmlNewParserCtxt();
    xmlDocPtr d = xmlCtxtReadMemory(c, s.data(), static_cast<int>(s.size()), "x.xml", "UTF-8", XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    bool ok = d && c->wellFormed && c->nsWellFormed;
    if (d) xmlFreeDoc(d);
    xmlFreeParserCtxt(c);
    return ok;
}

std::vector<std::string> allMatches(const std::string& s, const std::string& re) {
    std::vector<std::string> out;
    std::regex r(re);
    for (auto it = std::sregex_iterator(s.begin(), s.end(), r); it != std::sregex_iterator(); ++it) out.push_back((*it)[1].str());
    return out;
}

void appendBytes(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), static_cast<size_t>(size)); }

std::string makePng(int w, int h, bool noise, unsigned seed = 1) {
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
    std::mt19937 rng(seed);
    for (size_t i = 0; i < px.size(); ++i) px[i] = noise ? static_cast<unsigned char>(rng()) : static_cast<unsigned char>(i % 251);
    std::string out;
    stbi_write_png_to_func(appendBytes, &out, w, h, 3, px.data(), w * 3);
    return out;
}

const std::string IMG = "https://upload.wikimedia.org/steam.png";

Article article(const std::string& title, const std::string& body) {
    Article a;
    a.id = "id-" + title;
    a.title = title;
    a.capturedAt = "2026-10-10T08:00:00Z";
    std::string u = title;
    std::replace(u.begin(), u.end(), ' ', '_');
    a.sourceUrl = "https://en.wikipedia.org/wiki/" + u;
    a.siteName = "Wikipedia";
    a.bodyHtml = body;
    a.wordCount = 300;
    a.inputKind = InputKind::Wikipedia;
    return a;
}

PackPage page(const std::string& title, const std::string& fileName, const std::string& body, int depth = 1) {
    PackPage p;
    p.article = article(title, body);
    p.candidate = {title, title, p.article->sourceUrl, depth, "", 1, true};
    p.state = PackPageState::Done;
    p.fileName = fileName;
    return p;
}

Pack samplePack() {
    Pack pack;
    pack.id = "7c9e6679-7425-40de-944b-e07fc1f90ae7";
    pack.kind = PackSourceKind::Wikipedia;
    pack.title = "Steam engine pack";
    pack.rootKey = "Steam engine";
    pack.rootUrl = "https://en.wikipedia.org/wiki/Steam_engine";
    pack.limits.requestDelayMs = 0;
    pack.pages.push_back(page("Steam engine", "steam_engine.xhtml",
                              "<p>A steam engine uses a <a href=\"boiler.xhtml\">boiler</a> and a <a href=\"piston.xhtml#History\">piston</a>.</p>"
                              "<figure><img src=\"" + IMG + "\" alt=\"Engine\"/><figcaption>An engine</figcaption></figure>"
                              "<h2 id=\"Design\">Design</h2><p>Invented by <a class=\"ext\" href=\"https://en.wikipedia.org/wiki/James_Watt\">James Watt"
                              "<span class=\"ext\">\xE2\x86\x97</span></a>. See <a href=\"https://example.org/ref\">a reference</a>.</p>"
                              "<dl><dt>Pseudo heading</dt></dl><h3 id=\"Valves\">Valves</h3><p>Valves move.</p>",
                              0));
    pack.pages.push_back(page("Boiler", "boiler.xhtml", "<p>A boiler heats water for the <a href=\"steam_engine.xhtml\">steam engine</a>.</p><h2 id=\"Types\">Types</h2><p>Fire tube.</p>"));
    pack.pages.push_back(page("Piston", "piston.xhtml", "<p>A piston moves.</p><h2 id=\"History\">History</h2><p>Old.</p>"));
    PackPage failed;
    failed.candidate = {"Condenser", "Condenser", "https://en.wikipedia.org/wiki/Condenser", 1, "", 1, true};
    failed.state = PackPageState::Failed;
    failed.error = "HTTP 500";
    pack.pages.push_back(failed);
    PackPage skipped;
    skipped.candidate = {"Valve gear", "Valve gear", "https://en.wikipedia.org/wiki/Valve_gear", 1, "", 1, false};
    skipped.state = PackPageState::Skipped;
    pack.pages.push_back(skipped);
    pack.externalLinks.push_back({"James Watt", "James Watt", "https://en.wikipedia.org/wiki/James_Watt", "Steam engine"});
    return pack;
}

BuildOptions opts() {
    BuildOptions o;
    o.style = preset(StylePreset::Comfortable);
    return o;
}

void testFullPack() {
    std::vector<PackProgress> events;
    ImageMap images = {{IMG, makePng(64, 48, false)}};
    OutputFile f = buildPackEpub(samplePack(), opts(), images, [&](const PackProgress& p) { events.push_back(p); });
    writeFile(std::string(KS_TEST_OUT_DIR) + "/pack-sample.epub", f.bytes);
    CHECK_EQ(f.name, std::string("Steam engine pack.epub"));
    CHECK_EQ(f.mime, std::string("application/epub+zip"));
    CHECK(f.estimatedPages >= 3);
    auto z = readZip(f.bytes);
    CHECK(!z.empty() && z[0].name == "mimetype" && z[0].method == 0 && z[0].data == "application/epub+zip");
    CHECK(f.bytes.compare(30, 8, "mimetype") == 0);
    for (const auto& e : z) CHECK(e.name.back() != '/');   // No directory entries.
    CHECK(has(text(z, "META-INF/container.xml"), "full-path=\"EPUB/package.opf\""));
    for (const char* p : {"EPUB/nav.xhtml", "EPUB/toc.ncx", "EPUB/css/krosspoint.css", "EPUB/articles/index.xhtml",
                          "EPUB/articles/steam_engine.xhtml", "EPUB/articles/boiler.xhtml", "EPUB/articles/piston.xhtml",
                          "EPUB/articles/external-links.xhtml", "EPUB/articles/missing.xhtml", "EPUB/articles/attribution.xhtml",
                          "EPUB/images/steam_engine_01.jpg"})
        CHECK(find(z, p) != nullptr);

    // OPF metadata and manifest.
    std::string opf = text(z, "EPUB/package.opf");
    CHECK(wellFormed(opf));
    CHECK(has(opf, "<dc:title>Steam engine pack</dc:title>"));
    CHECK(has(opf, "<dc:creator id=\"creator\">Wikipedia contributors</dc:creator>"));
    CHECK(has(opf, "<dc:source>https://en.wikipedia.org/wiki/Steam_engine</dc:source>"));
    auto ids = allMatches(opf, "<item id=\"([^\"]+)\"");
    CHECK_EQ(std::set<std::string>(ids.begin(), ids.end()).size(), ids.size());
    for (const auto& href : allMatches(opf, "<item id=\"[^\"]+\" href=\"([^\"]+)\"")) CHECK(find(z, "EPUB/" + href) != nullptr);

    // Every XHTML file parses as XML, has unique ids and no remote image.
    for (const auto& e : z) {
        if (e.name.size() < 6 || e.name.compare(e.name.size() - 6, 6, ".xhtml") != 0) continue;
        bool ok = wellFormed(e.data);
        if (!ok) std::fprintf(stderr, "not well-formed: %s\n", e.name.c_str());
        CHECK(ok);
        auto xids = allMatches(e.data, " id=\"([^\"]+)\"");
        CHECK_EQ(std::set<std::string>(xids.begin(), xids.end()).size(), xids.size());
        CHECK(!has(e.data, "src=\"http"));
    }

    // Nav and NCX.
    std::string nav = text(z, "EPUB/nav.xhtml");
    for (const char* label : {"Title page", "Steam engine", "Boiler", "Piston", "External links", "Missing linked articles", "Sources and attribution"})
        CHECK(has(nav, std::string(">") + label + "</a>"));
    CHECK(has(nav, "href=\"articles/steam_engine.xhtml#Design\""));
    CHECK(has(nav, "href=\"articles/steam_engine.xhtml#Valves\""));
    std::string ncx = text(z, "EPUB/toc.ncx");
    CHECK(wellFormed(ncx));
    auto orders = allMatches(ncx, "playOrder=\"([0-9]+)\"");
    std::vector<int> nums;
    for (const auto& o : orders) nums.push_back(std::stoi(o));
    std::sort(nums.begin(), nums.end());
    bool gapFree = !nums.empty();
    for (size_t i = 0; i < nums.size(); ++i) gapFree = gapFree && nums[i] == static_cast<int>(i + 1);
    CHECK(gapFree);
    auto np = allMatches(ncx, "<navPoint id=\"([^\"]+)\"");
    CHECK_EQ(std::set<std::string>(np.begin(), np.end()).size(), np.size());
    CHECK_EQ(np.size(), 11u);   // 7 top-level points + Design, Valves, Types, History.
    // Article content.
    std::string steam = text(z, "EPUB/articles/steam_engine.xhtml");
    CHECK(has(steam, "<h1>Steam engine</h1>"));
    CHECK(has(steam, "class=\"source\">Source: Wikipedia, captured 2026-10-10</p>"));
    CHECK(has(steam, "href=\"boiler.xhtml\""));
    CHECK(has(steam, "href=\"piston.xhtml#History\""));
    CHECK(has(steam, "<a href=\"https://en.wikipedia.org/wiki/James_Watt\" class=\"ext\">"));
    CHECK(has(steam, "<span class=\"ext\">\xE2\x86\x97</span>"));
    CHECK(has(steam, "href=\"https://example.org/ref\""));
    CHECK(has(steam, "src=\"../images/steam_engine_01.jpg\""));
    CHECK(has(steam, "href=\"../css/krosspoint.css\""));
    CHECK(has(steam, "<p><b>Pseudo heading</b></p>"));
    CHECK(!has(steam, "<dl"));

    std::string attr = text(z, "EPUB/articles/attribution.xhtml");
    for (const char* t : {"Steam_engine", "Boiler", "Piston"}) CHECK(has(attr, std::string("href=\"https://en.wikipedia.org/wiki/") + t + "\""));
    CHECK(has(attr, "Creative Commons Attribution-ShareAlike 4.0 License"));
    CHECK(has(attr, "Generated by k-rosspointSender"));
    CHECK(has(attr, "captured 2026-10-10"));
    std::string missing = text(z, "EPUB/articles/missing.xhtml");
    CHECK(has(missing, "https://en.wikipedia.org/wiki/James_Watt"));
    CHECK(has(missing, "Condenser</a> (fetch failed)"));
    CHECK(has(missing, "Valve gear</a> (skipped)"));
    std::string ext = text(z, "EPUB/articles/external-links.xhtml");
    CHECK(has(ext, "<h2><a href=\"steam_engine.xhtml\">Steam engine</a></h2>"));
    CHECK(has(ext, "James Watt"));
    CHECK(has(text(z, "EPUB/articles/index.xhtml"), "href=\"boiler.xhtml\""));
    CHECK_EQ(std::count_if(events.begin(), events.end(), [](const PackProgress& p) { return p.phase == PackPhase::Building; }), 3);
    CHECK(!events.empty() && events.back().phase == PackPhase::Complete);
    CHECK(!events.empty() && events.back().errors.size() == 1 && events.back().errors[0].key == "Condenser");
}

void testVariants() {
    // maxImagesPerArticle.
    Pack pack = samplePack();
    pack.limits.maxImagesPerArticle = 1;
    pack.pages[0].article->bodyHtml = "<p>x</p><img src=\"" + IMG + "\" alt=\"a\"/><img src=\"" + IMG + "2\" alt=\"b\"/>";
    ImageMap images = {{IMG, makePng(32, 32, false)}, {IMG + "2", makePng(32, 32, false)}};
    auto z = readZip(buildPackEpub(pack, opts(), images).bytes);
    CHECK(find(z, "EPUB/images/steam_engine_01.jpg") != nullptr);
    CHECK(find(z, "EPUB/images/steam_engine_02.jpg") == nullptr);
    CHECK(has(text(z, "EPUB/articles/steam_engine.xhtml"), "class=\"img-alt\">b</p>"));

    // Author and web packs.
    pack = samplePack();
    pack.author = "Kobe";
    CHECK(has(text(readZip(buildPackEpub(pack, opts(), {}).bytes), "EPUB/package.opf"), "<dc:creator id=\"creator\">Kobe</dc:creator>"));
    pack = samplePack();
    pack.kind = PackSourceKind::Web;
    z = readZip(buildPackEpub(pack, opts(), {}).bytes);
    CHECK(has(text(z, "EPUB/package.opf"), "<dc:creator id=\"creator\">Various</dc:creator>"));
    CHECK(!has(text(z, "EPUB/articles/attribution.xhtml"), "ShareAlike"));

    // Pages without fileName: the builder calls rewriteLinks.
    pack = samplePack();
    for (auto& p : pack.pages) p.fileName.clear();
    pack.pages[1].article->bodyHtml = "<p>Back to the <a href=\"https://en.wikipedia.org/wiki/Steam_engine\">engine</a>.</p>";
    z = readZip(buildPackEpub(pack, opts(), {}).bytes);
    CHECK(find(z, "EPUB/articles/boiler.xhtml") != nullptr);
    CHECK(has(text(z, "EPUB/articles/boiler.xhtml"), "href=\"steam_engine.xhtml\""));
}

void testSizeLimit() {
    // Large noise images: the builder drops them to stay under 1 MB.
    Pack pack = samplePack();
    pack.limits.maxImagesPerArticle = 5;
    pack.limits.maxTotalEpubSizeMB = 1;
    ImageMap images;
    std::string body = "<p>Big images.</p>";
    for (int i = 0; i < 5; ++i) {
        std::string src = IMG + std::to_string(i);
        images[src] = makePng(480, 1600, true, static_cast<unsigned>(i + 7));
        body += "<img src=\"" + src + "\" alt=\"Engine " + std::to_string(i) + "\"/>";
    }
    pack.pages[0].article->bodyHtml = body;
    std::vector<PackProgress> events;
    OutputFile f = buildPackEpub(pack, opts(), images, [&](const PackProgress& p) { events.push_back(p); });
    CHECK(f.bytes.size() < 1024u * 1024u);
    auto z = readZip(f.bytes);
    CHECK(find(z, "EPUB/images/steam_engine_01.jpg") == nullptr);
    CHECK(!has(text(z, "EPUB/package.opf"), "images/"));
    CHECK(has(text(z, "EPUB/articles/steam_engine.xhtml"), "class=\"img-alt\">Engine 0</p>"));
    CHECK(!events.empty() && has(events.back().message, "Removed the images"));

    // Still too large without images: a clear error.
    pack = samplePack();
    pack.limits.maxTotalEpubSizeMB = 1;
    std::mt19937 rng(3);
    std::string words;
    while (words.size() < 2600000) {
        words += "<p>";
        for (int w = 0; w < 40; ++w) {
            for (int c = 0; c < 6; ++c) words += static_cast<char>('a' + rng() % 26);
            words += ' ';
        }
        words += "</p>";
    }
    pack.pages[1].article->bodyHtml = words;
    std::string message;
    try {
        buildPackEpub(pack, opts(), {});
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(std::regex_match(message, std::regex(R"(^Pack is [\d.]+ MB; the limit is 1 MB\. Lower the article count or the image limit\.$)")));
}

void testDefinitionLists() {
    Article a = article("Lists", "<p>Intro.</p><dl><dt>Pseudo heading</dt></dl><p>Text under it.</p>"
                                 "<dl><dt>Term A</dt><dd>Def A</dd><dt>Orphan term</dt></dl><dl><dd>Indented quote</dd></dl>"
                                 "<dl><dt>Two</dt><dt>Terms</dt><dd>One def</dd><dd>Two defs</dd></dl>"
                                 "<dl><dt>Outer</dt><dd><dl><dt>Inner heading</dt></dl></dd></dl>");
    BuildOptions o = opts();
    o.includeCover = false;
    auto z = readZip(buildEpub(a, o).bytes);
    std::string ch = text(z, "OEBPS/chapter-001.xhtml");
    CHECK(wellFormed(ch));
    // Every dl starts with dt, ends with dd, and has a dd.
    std::regex dlRe("<dl>(.*?)</dl>");
    int lists = 0;
    for (auto it = std::sregex_iterator(ch.begin(), ch.end(), dlRe); it != std::sregex_iterator(); ++it) {
        std::string inner = (*it)[1].str();
        if (has(inner, "<dl>")) continue;   // Nested match; the inner list is checked on its own.
        ++lists;
        CHECK(inner.rfind("<dt", 0) == 0);
        CHECK(has(inner, "<dd>"));
        CHECK(inner.size() >= 5 && inner.compare(inner.size() - 5, 5, "</dd>") == 0);
    }
    CHECK(lists >= 3);
    for (const char* b : {"<p><b>Pseudo heading</b></p>", "<p><b>Orphan term</b></p>", "<p><b>Inner heading</b></p>"}) CHECK(has(ch, b));
    CHECK(ch.find("Pseudo heading") < ch.find("Text under it."));
    CHECK(has(ch, "Indented quote") && has(ch, "Two defs"));
    CHECK(has(ch, "<dt/>") || has(ch, "<dt></dt>"));
}

} // namespace

int main() {
    testFullPack();
    testVariants();
    testSizeLimit();
    testDefinitionLists();
    TEST_MAIN_END();
}
