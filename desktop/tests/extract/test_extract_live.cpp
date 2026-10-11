// Live network check for the extract module. It runs only when the
// environment variable KS_LIVE is set. Otherwise it skips and passes.
#include "check.h"
#include "extract/extract.h"

#include <libxml/parser.h>

#include <cstdlib>

using namespace ks;
using namespace ks::extract;

int main() {
    if (!std::getenv("KS_LIVE")) {
        std::printf("KS_LIVE is not set. Skip the live check.\n");
        return 0;
    }
    FetchResult r = fetchWikipedia("https://en.wikipedia.org/wiki/E_Ink");
    if (!r.ok) std::fprintf(stderr, "fetchWikipedia failed: %s (HTTP %d)\n", r.error.c_str(), r.status);
    CHECK(r.ok);
    if (r.ok) {
        RawInput in;
        in.kind = InputKind::Wikipedia;
        in.content = r.body;
        in.sourceUrl = r.finalUrl;
        Article a = extractArticle(in);
        std::printf("finalUrl: %s\ntitle: %s\nwords: %d\nimages: %zu\nheadings: %zu\n", r.finalUrl.c_str(), a.title.c_str(), a.wordCount,
                    a.images.size(), a.toc.size());
        CHECK(a.title == "E Ink");
        CHECK(a.wordCount > 300);
        std::string doc = "<root>" + a.bodyHtml + "</root>";
        xmlDoc* d = xmlReadMemory(doc.data(), static_cast<int>(doc.size()), nullptr, "UTF-8", XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
        CHECK(d != nullptr);
        if (d) xmlFreeDoc(d);
        ImageMap m = fetchImages(a);
        std::printf("images fetched: %zu of %zu\n", m.size(), a.images.size());
        CHECK(a.images.empty() || !m.empty());
    }
    FetchResult p = fetchUrl("https://example.com/");
    std::printf("example.com: ok=%d status=%d bytes=%zu\n", p.ok ? 1 : 0, p.status, p.body.size());
    CHECK(p.ok);
    TEST_MAIN_END();
}
