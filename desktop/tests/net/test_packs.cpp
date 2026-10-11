// Tests for the pack storage of src/net (packs/<id>/pack.json + images/),
// the packLimits field of settings.json, and clearLibrary() with packs.
#include "net/net.h"
#include "check.h"

#include <cstdlib>
#include <filesystem>

using namespace ks;
namespace fs = std::filesystem;

static Article makeArticle(const std::string& title) {
    Article a;
    a.id = newUuid();
    a.title = title;
    a.author = "Wikipedia contributors";
    a.capturedAt = "2026-10-10T10:00:00Z";
    a.sourceUrl = "https://en.wikipedia.org/wiki/" + title;
    a.siteName = "Wikipedia";
    a.language = "en";
    a.tags = {"a", "b"};
    a.bodyHtml = "<p>Text of " + title + " &amp; more.</p><h2 id=\"sec-1\">Part</h2>";
    a.toc = {{"sec-1", 2, "Part"}};
    a.images = {{"https://upload.wikimedia.org/x.jpg", "an image"}};
    a.wordCount = 321;
    a.inputKind = InputKind::Wikipedia;
    return a;
}

static Pack makePack(const std::string& title, const std::string& updatedAt) {
    Pack p;
    p.id = newUuid();
    p.kind = PackSourceKind::Wikipedia;
    p.title = title;
    p.author = "Me";
    p.rootKey = "Steam engine";
    p.rootUrl = "https://en.wikipedia.org/wiki/Steam_engine";
    p.limits.depth = 2;
    p.limits.maxArticles = 7;
    p.limits.maxImagesPerArticle = 3;
    p.limits.maxTotalEpubSizeMB = 40;
    p.limits.requestDelayMs = 250;
    p.limits.includeReferences = true;
    p.limits.includeInfobox = true;
    p.limits.sameDomainOnly = false;
    PackPage root;
    root.candidate = {"Steam engine", "Steam engine", p.rootUrl, 0, "", 0, true, false, false, false};
    root.state = PackPageState::Done;
    root.article = makeArticle("Steam engine");
    root.fileName = "steam_engine.xhtml";
    PackPage failed;
    failed.candidate = {"Boiler", "Boiler", "https://en.wikipedia.org/wiki/Boiler", 1, "Steam engine", 4, true, true, true, true};
    failed.state = PackPageState::Failed;
    failed.error = "HTTP 404";
    PackPage queued;
    queued.candidate = {"Piston", "Piston", "https://en.wikipedia.org/wiki/Piston", 2, "Boiler", 1, false, false, false, false};
    p.pages = {root, failed, queued};
    p.externalLinks = {{"Watt", "James Watt", "https://en.wikipedia.org/wiki/James_Watt", "Steam engine"}};
    p.createdAt = "2026-10-10T09:00:00Z";
    p.updatedAt = updatedAt;
    return p;
}

static void testPackRoundTrip() {
    using namespace ks::net;
    Pack p = makePack("Steam pack", "2026-10-10T12:00:00Z");
    ImageMap imgs = {{"https://upload.wikimedia.org/x.jpg", std::string("\xff\xd8\x00jpeg", 7)}};
    savePack(p, &imgs);
    auto r = getPack(p.id);
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(r->id, p.id);
    CHECK(r->kind == PackSourceKind::Wikipedia);
    CHECK_EQ(r->title, p.title);
    CHECK_EQ(r->author, p.author);
    CHECK_EQ(r->rootKey, p.rootKey);
    CHECK_EQ(r->rootUrl, p.rootUrl);
    CHECK_EQ(r->limits.depth, 2);
    CHECK_EQ(r->limits.maxArticles, 7);
    CHECK_EQ(r->limits.maxImagesPerArticle, 3);
    CHECK_EQ(r->limits.maxTotalEpubSizeMB, 40);
    CHECK_EQ(r->limits.requestDelayMs, 250);
    CHECK(r->limits.includeReferences && r->limits.includeInfobox && !r->limits.sameDomainOnly);
    CHECK_EQ(r->pages.size(), size_t(3));
    CHECK_EQ(r->createdAt, p.createdAt);
    CHECK_EQ(r->updatedAt, p.updatedAt);
    if (r->pages.size() == 3) {
        const PackPage& a = r->pages[0];
        CHECK(a.state == PackPageState::Done);
        CHECK(a.article.has_value());
        CHECK_EQ(a.fileName, std::string("steam_engine.xhtml"));
        if (a.article) {
            CHECK_EQ(a.article->bodyHtml, p.pages[0].article->bodyHtml);
            CHECK_EQ(a.article->wordCount, 321);
            CHECK_EQ(a.article->toc.size(), size_t(1));
            CHECK_EQ(a.article->images.size(), size_t(1));
            CHECK(a.article->inputKind == InputKind::Wikipedia);
            CHECK(a.article->tags == p.pages[0].article->tags);
        }
        const PackPage& b = r->pages[1];
        CHECK(b.state == PackPageState::Failed);
        CHECK_EQ(b.error, std::string("HTTP 404"));
        CHECK(!b.article.has_value());
        CHECK_EQ(b.candidate.depth, 1);
        CHECK_EQ(b.candidate.parentKey, std::string("Steam engine"));
        CHECK_EQ(b.candidate.linkCount, 4);
        CHECK(b.candidate.selected && b.candidate.disambiguation && b.candidate.redirect && b.candidate.manual);
        CHECK(r->pages[2].state == PackPageState::Queued);
        CHECK(!r->pages[2].candidate.selected);
    }
    CHECK_EQ(r->externalLinks.size(), size_t(1));
    if (!r->externalLinks.empty()) {
        CHECK_EQ(r->externalLinks[0].title, std::string("James Watt"));
        CHECK_EQ(r->externalLinks[0].fromKey, std::string("Steam engine"));
    }
    CHECK(loadPackImages(p.id) == imgs);

    // Save without images keeps the stored images. The record survives a save.
    updatePackRecord(p.id, "steam.epub", std::nullopt);
    TransferRecord t;
    t.deviceName = "X4";
    t.ok = true;
    t.fileName = "steam.epub";
    updatePackRecord(p.id, "", t);
    p.title = "Renamed";
    savePack(p);
    CHECK(loadPackImages(p.id) == imgs);
    CHECK_EQ(getPack(p.id)->title, std::string("Renamed"));
    PackRecord rec = getPackRecord(p.id);
    CHECK_EQ(rec.fileName, std::string("steam.epub"));
    CHECK_EQ(rec.transfers.size(), size_t(1));
    if (!rec.transfers.empty()) CHECK(rec.transfers[0].ok && !rec.transfers[0].id.empty());

    // An empty map removes the images.
    ImageMap none;
    savePack(p, &none);
    CHECK(loadPackImages(p.id).empty());
}

static void testPackList() {
    using namespace ks::net;
    clearLibrary();
    CHECK(listPacks().empty());
    Pack older = makePack("Older", "2026-10-09T00:00:00Z");
    Pack newer = makePack("Newer", "2026-10-10T00:00:00Z");
    savePack(older);
    savePack(newer);
    auto list = listPacks();
    CHECK_EQ(list.size(), size_t(2));
    if (list.size() == 2) {
        CHECK_EQ(list[0].title, std::string("Newer"));
        CHECK_EQ(list[1].title, std::string("Older"));
    }
    deletePack(older.id);
    CHECK(!getPack(older.id).has_value());
    CHECK_EQ(listPacks().size(), size_t(1));

    // Bad ids.
    Pack bad = makePack("Bad", "");
    bad.id = "../evil";
    bool threw = false;
    try {
        savePack(bad);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    bad.id.clear();
    threw = false;
    try {
        savePack(bad);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(!getPack("../evil").has_value());
    CHECK(loadPackImages("..").empty());

    // A broken pack.json is skipped.
    makeDirs(joinPath(joinPath(dataDir(), "packs"), "broken"));
    writeFile(joinPath(joinPath(joinPath(dataDir(), "packs"), "broken"), "pack.json"), "{nope");
    CHECK_EQ(listPacks().size(), size_t(1));

    // clearLibrary removes the packs too.
    LibraryEntry e;
    e.article = makeArticle("Lib");
    saveEntry(e);
    clearLibrary();
    CHECK(listPacks().empty());
    CHECK(listEntries().empty());
}

static void testPackLimitsSettings() {
    using namespace ks::net;
    AppSettings s;
    s.packLimits.depth = 0;
    s.packLimits.maxArticles = 55;
    s.packLimits.maxImagesPerArticle = 0;
    s.packLimits.maxTotalEpubSizeMB = 12;
    s.packLimits.requestDelayMs = 1500;
    s.packLimits.includeReferences = true;
    s.packLimits.includeInfobox = true;
    s.packLimits.sameDomainOnly = false;
    saveSettings(s);
    AppSettings r = loadSettings();
    CHECK_EQ(r.packLimits.depth, 0);
    CHECK_EQ(r.packLimits.maxArticles, 55);
    CHECK_EQ(r.packLimits.maxImagesPerArticle, 0);
    CHECK_EQ(r.packLimits.maxTotalEpubSizeMB, 12);
    CHECK_EQ(r.packLimits.requestDelayMs, 1500);
    CHECK(r.packLimits.includeReferences && r.packLimits.includeInfobox && !r.packLimits.sameDomainOnly);
    // Out-of-range values are clamped. A missing block gives the defaults.
    writeFile(joinPath(dataDir(), "settings.json"), R"({"packLimits": {"depth": 9, "maxArticles": 0}})");
    r = loadSettings();
    CHECK_EQ(r.packLimits.depth, 2);
    CHECK_EQ(r.packLimits.maxArticles, 1);
    CHECK_EQ(r.packLimits.maxImagesPerArticle, 2);
    writeFile(joinPath(dataDir(), "settings.json"), R"({"helperPort": 8200})");
    CHECK_EQ(loadSettings().packLimits.maxArticles, 20);
}

int main() {
    if (!std::getenv("KS_DATA_DIR")) {
        std::fprintf(stderr, "KS_DATA_DIR is not set. Run this test through ctest.\n");
        return 1;
    }
    std::error_code ec;
    fs::remove_all(fs::u8path(dataDir()), ec);
    testPackRoundTrip();
    testPackList();
    testPackLimitsSettings();
    TEST_MAIN_END();
}
