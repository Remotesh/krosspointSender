// Live network check for the spider and the pack builder. It runs only when the
// environment variable KS_LIVE is set. Otherwise it skips and passes.
// Scan: https://en.wikipedia.org/wiki/Steam_engine, depth 1, max 5 articles.
// Output: <build>/pack-live.epub.
#include "check.h"
#include "output/output.h"
#include "spider/spider.h"

#include <chrono>
#include <cstdlib>

using namespace ks;

int main() {
    if (!std::getenv("KS_LIVE")) {
        std::printf("KS_LIVE is not set. Skip the live check.\n");
        return 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    auto root = spider::normalizeWikipediaTitle("https://en.wikipedia.org/wiki/Steam_engine");
    CHECK(root.has_value());
    if (!root) TEST_MAIN_END();
    PackLimits limits;
    limits.depth = 1;
    limits.maxArticles = 5;
    auto log = [](const PackProgress& p) {
        if (!p.message.empty()) std::printf("  [%s] %d/%d %s: %s\n", toString(p.phase).c_str(), p.done, p.total, p.currentTitle.c_str(), p.message.c_str());
    };
    std::vector<PackCandidate> cands;
    try {
        cands = spider::scanWikipediaLinks(*root, limits, log);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "scan failed: %s\n", e.what());
    }
    int selected = 0, disambig = 0, redirects = 0;
    for (const auto& c : cands) {
        selected += c.selected ? 1 : 0;
        disambig += c.disambiguation ? 1 : 0;
        redirects += c.redirect ? 1 : 0;
    }
    std::printf("candidates: %zu, selected: %d, disambiguation: %d, redirect: %d\n", cands.size(), selected, disambig, redirects);
    CHECK(!cands.empty() && cands[0].key == "Steam engine" && cands[0].selected);
    CHECK(cands.size() <= 25u);
    CHECK_EQ(selected, 5);

    Pack pack = spider::newPack(PackSourceKind::Wikipedia, root->title, root->url, "Steam engine (pack)", limits);
    spider::fillPagesFromCandidates(pack, cands);
    pack = spider::fetchPackPages(pack, {}, log);
    int done = 0;
    ImageMap images;
    for (const auto& p : pack.pages) {
        if (p.state == PackPageState::Done) {
            ++done;
            ImageMap m = extract::fetchImages(*p.article);
            images.insert(m.begin(), m.end());
            std::printf("  page %-30s words %5d images %zu\n", p.candidate.title.c_str(), p.article->wordCount, p.article->images.size());
        } else if (p.state == PackPageState::Failed) {
            std::printf("  FAILED %s: %s\n", p.candidate.title.c_str(), p.error.c_str());
        }
    }
    CHECK_EQ(done, 5);
    spider::Rewritten rw = spider::rewriteLinks(pack.pages, pack.kind);
    pack.pages = rw.pages;
    pack.externalLinks = rw.external;
    std::printf("external links: %zu, images fetched: %zu\n", rw.external.size(), images.size());
    try {
        BuildOptions o;
        OutputFile f = output::buildPackEpub(pack, o, images, log);
        std::string path = std::string(KS_TEST_OUT_DIR) + "/pack-live.epub";
        CHECK(writeFile(path, f.bytes));
        std::printf("EPUB: %s (%zu bytes, ~%d pages)\n", path.c_str(), f.bytes.size(), f.estimatedPages);
        CHECK(f.bytes.size() > 10000u);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "build failed: %s\n", e.what());
        CHECK(false);
    }
    std::printf("time: %lld s\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count()));
    TEST_MAIN_END();
}
