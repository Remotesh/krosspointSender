// Tests for the spider scan and the page fetch (port of tests/spider/wikipedia.test.ts,
// pack.test.ts and followup.test.ts of the web app). The HTTP layer is the fake API.
#include "spider_test_util.h"

#include <algorithm>
#include <thread>

using namespace ks;
using namespace ks::spider;
using namespace sptest;

namespace {

std::vector<std::string> keys(const std::vector<PackCandidate>& v) {
    std::vector<std::string> out;
    for (const auto& c : v) out.push_back(c.key);
    return out;
}

std::vector<std::string> selectedKeys(const std::vector<PackCandidate>& v) {
    std::vector<std::string> out;
    for (const auto& c : v)
        if (c.selected) out.push_back(c.key);
    return out;
}

using VS = std::vector<std::string>;
const VS kExpectedBody = {"Heat engine", "Work (physics)", "Steam", "Mercury (disambiguation)", "Steam engines", "Thomas Newcomen", "James Watt"};

std::vector<LinkMeta> metaOf(const json& parse) {
    std::vector<LinkMeta> out;
    for (const auto& l : parse["links"]) out.push_back({l["ns"].get<int>(), l["title"].get<std::string>(), l.value("exists", true)});
    return out;
}

void testNormalize() {
    auto r = normalizeWikipediaTitle("https://en.wikipedia.org/wiki/Steam_engine");
    CHECK(r && r->lang == "en" && r->title == "Steam engine" && r->url == "https://en.wikipedia.org/wiki/Steam_engine");
    r = normalizeWikipediaTitle("steam_engine");
    CHECK(r && r->title == "Steam engine");
    r = normalizeWikipediaTitle("  Heat   engine ");
    CHECK(r && r->title == "Heat engine");
    r = normalizeWikipediaTitle("Dampfmaschine", "de");
    CHECK(r && r->lang == "de" && r->url == "https://de.wikipedia.org/wiki/Dampfmaschine");
    r = normalizeWikipediaTitle("https://de.m.wikipedia.org/wiki/Dampfmaschine#Geschichte");
    CHECK(r && r->lang == "de" && r->title == "Dampfmaschine");
    r = normalizeWikipediaTitle("en.m.wikipedia.org/wiki/Heat_engine");
    CHECK(r && r->title == "Heat engine");
    r = normalizeWikipediaTitle("https://fr.wikipedia.org/wiki/Machine_%C3%A0_vapeur");
    CHECK(r && r->title == "Machine \xC3\xA0 vapeur" && r->url == "https://fr.wikipedia.org/wiki/Machine_%C3%A0_vapeur");
    CHECK(!normalizeWikipediaTitle(""));
    CHECK(!normalizeWikipediaTitle("https://example.com/wiki/X"));
    CHECK(!normalizeWikipediaTitle("Help:Contents"));
}

void testBodyLinks() {
    json root = json::parse(fixture("parse-steam-engine.json"));
    CHECK(extractBodyLinks(root["parse"]["text"], "en", "Steam engine", metaOf(root["parse"])) == kExpectedBody);
    // The ns number from prop=links is a second check.
    std::string html = "<div class=\"mw-parser-output\"><p><a href=\"/wiki/Foo:Bar\">x</a> <a href=\"/wiki/Ok\">ok</a></p></div>";
    CHECK(extractBodyLinks(html, "en", "Root", {{4, "Foo:Bar", true}, {0, "Ok", true}}) == VS{"Ok"});
    // Body-text links rank before caption links.
    json cap = json::parse(fixture("parse-caption-redirect.json"));
    CHECK(extractBodyLinks(cap["parse"]["text"], "en", "Steam engine", metaOf(cap["parse"])) ==
          (VS{"Heat engine", "Steam", "Stott Park Bobbin Mill", "Ljubljana, Slovenia"}));
    std::string html2 = "<div class=\"mw-parser-output\"><figure><figcaption><a href=\"/wiki/Steam\">steam</a></figcaption></figure>"
                        "<p><a href=\"/wiki/Heat_engine\">heat</a> <a href=\"/wiki/Steam\">steam</a></p></div>";
    CHECK(extractBodyLinks(html2, "en", "Root") == (VS{"Heat engine", "Steam"}));
}

void testScanDepth1() {
    json rootParse = json::parse(fixture("parse-steam-engine.json"));
    json rootProps = json::parse(fixture("pageprops-steam-engine.json"));
    FakeApi api;
    api.handler = [&](const std::string& url, int&) { return param(url, "action") == "parse" ? rootParse : rootProps; };
    api.install();
    clearAliases();
    std::vector<PackProgress> progress;
    PackLimits l = limits();
    l.maxArticles = 3;
    auto out = scanWikipediaLinks({"en", "steam_engine", ""}, l, [&](const PackProgress& p) { progress.push_back(p); });
    CHECK(keys(out) == (VS{"Steam engine", "Heat engine", "Work (physics)", "Steam", "Mercury (disambiguation)", "Thomas Newcomen", "James Watt"}));
    CHECK(out[0].depth == 0 && out[0].selected && out[0].url == "https://en.wikipedia.org/wiki/Steam_engine");
    CHECK(selectedKeys(out) == (VS{"Steam engine", "Heat engine", "Work (physics)"}));
    auto merc = std::find_if(out.begin(), out.end(), [](const PackCandidate& c) { return c.key == "Mercury (disambiguation)"; });
    CHECK(merc != out.end() && merc->disambiguation && !merc->selected);
    CHECK(std::all_of(out.begin() + 1, out.end(), [](const PackCandidate& c) { return c.depth == 1 && c.parentKey == "Steam engine" && c.linkCount == 1; }));
    CHECK_EQ(api.count(), 2u);
    CHECK(api.paramsOf("prop", "parse") == VS{"text|links"});
    CHECK(api.paramsOf("format") == (VS{"json", "json"}));
    CHECK(api.paramsOf("formatversion") == (VS{"2", "2"}));
    CHECK(api.paramsOf("ppprop", "query") == VS{"disambiguation"});
    VS titles = split(api.paramsOf("titles", "query")[0], '|');
    CHECK(std::find(titles.begin(), titles.end(), "Steam engines") != titles.end());
    CHECK(std::all_of(progress.begin(), progress.end(), [](const PackProgress& p) { return p.phase == PackPhase::Scanning; }));

    l.maxArticles = 6;
    out = scanWikipediaLinks({"en", "Steam engine", ""}, l, {});
    CHECK(selectedKeys(out) == (VS{"Steam engine", "Heat engine", "Work (physics)", "Steam", "Thomas Newcomen", "James Watt"}));
}

void testRedirectFlags() {
    json cap = json::parse(fixture("parse-caption-redirect.json"));
    FakeApi api;
    api.handler = [&](const std::string& url, int&) {
        return param(url, "action") == "parse" ? cap : withRedirect(url, "Ljubljana, Slovenia", "Ljubljana");
    };
    api.install();
    clearAliases();
    auto out = scanWikipediaLinks({"en", "Steam engine", ""}, limits(), {});
    CHECK(keys(out) == (VS{"Steam engine", "Heat engine", "Steam", "Stott Park Bobbin Mill", "Ljubljana"}));
    CHECK(out.size() == 5 && out[4].redirect && out[4].url == "https://en.wikipedia.org/wiki/Ljubljana");
    CHECK(std::none_of(out.begin(), out.begin() + 4, [](const PackCandidate& c) { return c.redirect || c.disambiguation; }));

    FakeApi api2;
    api2.handler = [&](const std::string& url, int&) {
        return param(url, "action") == "parse" ? parseBody("Root", {"Ljubljana, Slovenia", "Steam", "Ljubljana"})
                                               : withRedirect(url, "Ljubljana, Slovenia", "Ljubljana");
    };
    api2.install();
    out = scanWikipediaLinks({"en", "Root", ""}, limits(), {});
    CHECK(keys(out) == (VS{"Root", "Ljubljana", "Steam"}));
    CHECK(out.size() == 3 && !out[1].redirect);
}

void testBatchesCapDepth2() {
    VS many;
    for (int i = 0; i < 70; ++i) many.push_back("Page " + std::to_string(i));
    FakeApi api;
    api.handler = [&](const std::string& url, int&) { return param(url, "action") == "parse" ? parseBody("Root", many) : plainPageProps(url); };
    api.install();
    auto out = scanWikipediaLinks({"en", "Root", ""}, limits(), {});
    CHECK_EQ(out.size(), 71u);
    VS t = api.paramsOf("titles", "query");
    CHECK(t.size() == 2 && split(t[0], '|').size() == 50 && split(t[1], '|').size() == 21);

    // Depth 2 order and link counts.
    std::map<std::string, VS> pages = {{"Root", {"A", "B", "C"}}, {"A", {"B", "C", "D"}}, {"B", {"C", "Root"}}, {"C", {}}};
    FakeApi api2;
    api2.handler = [&](const std::string& url, int&) {
        if (param(url, "action") != "parse") return plainPageProps(url);
        return parseBody(param(url, "page"), pages[param(url, "page")]);
    };
    api2.install();
    PackLimits l = limits();
    l.depth = 2;
    out = scanWikipediaLinks({"en", "Root", ""}, l, {});
    CHECK(keys(out) == (VS{"Root", "A", "B", "C", "D"}));
    std::vector<int> depths, counts;
    for (const auto& c : out) {
        depths.push_back(c.depth);
        counts.push_back(c.linkCount);
    }
    CHECK(depths == (std::vector<int>{0, 1, 1, 1, 2}));
    CHECK(counts == (std::vector<int>{1, 1, 2, 3, 1}));
    CHECK(out.size() == 5 && out[4].parentKey == "A" && !out[4].selected);

    // Never more than maxArticles * 5 candidates.
    FakeApi api3;
    api3.handler = [&](const std::string& url, int&) {
        if (param(url, "action") != "parse") return plainPageProps(url);
        std::string page = param(url, "page");
        VS kids;
        for (int i = 0; i < 4; ++i) kids.push_back(page + " child " + std::to_string(i));
        return parseBody(page, kids);
    };
    api3.install();
    l.maxArticles = 2;
    out = scanWikipediaLinks({"en", "Root", ""}, l, {});
    CHECK_EQ(out.size(), 10u);
    CHECK_EQ(api3.count("parse"), 3u);   // Root (5), child 0 (9), child 1 (10 = cap).
}

void testDelayAndCancel() {
    std::map<std::string, VS> pages = {{"Root", {"A", "B"}}, {"A", {}}, {"B", {}}};
    FakeApi api;
    api.handler = [&](const std::string& url, int&) {
        if (param(url, "action") != "parse") return plainPageProps(url);
        return parseBody(param(url, "page"), pages[param(url, "page")]);
    };
    api.install();
    PackLimits l = limits();
    l.depth = 2;
    l.requestDelayMs = 150;
    scanWikipediaLinks({"en", "Root", ""}, l, {});
    CHECK_EQ(api.calls.size(), 4u);   // Root, A, B, pageprops.
    for (size_t i = 1; i < api.calls.size(); ++i)
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(api.calls[i].at - api.calls[i - 1].at).count() >= 145);

    // Cancel during the delay.
    FakeApi api2;
    api2.handler = api.handler;
    api2.install();
    l.requestDelayMs = 5000;
    bool cancel = false;
    std::vector<PackProgress> progress;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel = true;
    });
    auto start = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        scanWikipediaLinks({"en", "Root", ""}, l, [&](const PackProgress& p) { progress.push_back(p); }, &cancel);
    } catch (const CancelledError&) {
        threw = true;
    }
    t.join();
    CHECK(threw);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    CHECK_EQ(api2.calls.size(), 1u);
    CHECK(!progress.empty() && progress.back().phase == PackPhase::Cancelled);

    // Already cancelled: no request.
    FakeApi api3;
    api3.handler = api.handler;
    api3.install();
    threw = false;
    try {
        scanWikipediaLinks({"en", "Root", ""}, limits(), {}, &cancel);
    } catch (const CancelledError&) {
        threw = true;
    }
    CHECK(threw && api3.calls.empty());
}

void testFailuresDepth0Retry() {
    FakeApi api;
    api.handler = [&](const std::string& url, int&) -> json {
        if (param(url, "action") != "parse") return plainPageProps(url);
        std::string page = param(url, "page");
        if (page == "A") return {{"error", {{"code", "missingtitle"}, {"info", "The page does not exist."}}}};
        return parseBody(page, page == "Root" ? VS{"A", "B"} : VS{"Z"});
    };
    api.install();
    PackLimits l = limits();
    l.depth = 2;
    std::vector<PackProgress> progress;
    auto out = scanWikipediaLinks({"en", "Root", ""}, l, [&](const PackProgress& p) { progress.push_back(p); });
    CHECK(keys(out) == (VS{"Root", "A", "B", "Z"}));
    CHECK(progress.back().errors.size() == 1 && progress.back().errors[0].key == "A" &&
          progress.back().errors[0].error.find("does not exist") != std::string::npos);

    FakeApi api0;
    api0.handler = [](const std::string&, int&) { return json::object(); };
    api0.install();
    l.depth = 0;
    out = scanWikipediaLinks({"en", "root page", ""}, l, {});
    CHECK(out.size() == 1 && out[0].key == "Root page" && out[0].depth == 0 && out[0].selected);
    CHECK(api0.calls.empty());

    // HTTP 429 once: wait 2 s, report it, try again.
    int n = 0;
    FakeApi api2;
    api2.handler = [&](const std::string& url, int& status) -> json {
        if (param(url, "action") == "parse" && n++ == 0) {
            status = 429;
            return json::object();
        }
        return param(url, "action") == "parse" ? parseBody("Root", {"A"}) : plainPageProps(url);
    };
    api2.install();
    progress.clear();
    out = scanWikipediaLinks({"en", "Root", ""}, limits(), [&](const PackProgress& p) { progress.push_back(p); });
    CHECK(keys(out) == (VS{"Root", "A"}));
    CHECK(api2.count("parse") == 2);
    CHECK(api2.calls.size() >= 2 &&
          std::chrono::duration_cast<std::chrono::milliseconds>(api2.calls[1].at - api2.calls[0].at).count() >= 1990);
    CHECK(std::any_of(progress.begin(), progress.end(), [](const PackProgress& p) { return p.message == "rate limited, waiting 2s"; }));
}

json wikiText(const std::string& title) {
    std::string t = title;
    return {{"parse",
             {{"title", t},
              {"text", "<div class=\"mw-parser-output\"><p>" + t + " is a page with enough words to be a real paragraph of text for the test.</p>"
                       "<p><img src=\"//upload.wikimedia.org/" + t + "-1.jpg\" alt=\"one\"/></p><p><img src=\"//upload.wikimedia.org/" + t +
                       "-2.jpg\" alt=\"two\"/></p><p><img src=\"//upload.wikimedia.org/" + t + "-3.jpg\" alt=\"three\"/></p></div>"}}}};
}

PackPage queued(const std::string& key, int depth = 1, bool selected = true) {
    PackPage p;
    p.candidate = cand(key, wurl(key), depth, selected);
    return p;
}

void testFetchPackPages() {
    FakeApi api;
    api.handler = [&](const std::string& url, int&) -> json {
        std::string page = param(url, "page");
        if (page == "Broken") return {{"error", {{"code", "missingtitle"}, {"info", "The page you specified doesn't exist."}}}};
        return wikiText(page);
    };
    api.install();
    PackLimits l = limits();
    l.maxImagesPerArticle = 1;
    Pack pack = newPack(PackSourceKind::Wikipedia, "Root", wurl("Root"), "Root", l);
    CHECK_EQ(pack.id.size(), 36u);
    pack.pages = {queued("Alpha"), queued("Root", 0), queued("Broken"), queued("Unwanted", 1, false), queued("Beta")};
    std::vector<PackProgress> progress;
    Pack out = fetchPackPages(pack, {}, [&](const PackProgress& p) { progress.push_back(p); });
    CHECK(pack.pages[0].candidate.key == "Alpha" && pack.pages[0].state == PackPageState::Queued);   // Input not changed.
    VS order;
    std::vector<PackPageState> states;
    for (const auto& p : out.pages) {
        order.push_back(p.candidate.key);
        states.push_back(p.state);
    }
    CHECK(order == (VS{"Root", "Alpha", "Broken", "Unwanted", "Beta"}));
    CHECK(states == (std::vector<PackPageState>{PackPageState::Done, PackPageState::Done, PackPageState::Failed, PackPageState::Skipped,
                                                PackPageState::Done}));
    CHECK(api.paramsOf("page") == (VS{"Root", "Alpha", "Broken", "Beta"}));
    CHECK(out.pages[2].error.find("doesn't exist") != std::string::npos && !out.pages[2].article);
    const PackProgress& last = progress.back();
    CHECK(last.phase == PackPhase::Complete && last.done == 4 && last.total == 4);
    CHECK(last.errors.size() == 1 && last.errors[0].key == "Broken" && last.errors[0].error == out.pages[2].error);
    CHECK(std::any_of(progress.begin(), progress.end(),
                      [](const PackProgress& p) { return p.phase == PackPhase::Fetching && p.currentTitle == "Alpha" && p.done == 1; }));
    const Article& root = *out.pages[0].article;
    CHECK(root.title == "Root" && root.inputKind == InputKind::Wikipedia);
    CHECK_EQ(root.images.size(), 1u);
    size_t imgs = 0;
    for (size_t p = root.bodyHtml.find("<img"); p != std::string::npos; p = root.bodyHtml.find("<img", p + 1)) ++imgs;
    CHECK_EQ(imgs, 1u);

    // A page that is already done is not fetched again.
    FakeApi api2;
    api2.handler = [&](const std::string& url, int&) { return wikiText(param(url, "page")); };
    api2.install();
    Pack p2 = newPack(PackSourceKind::Wikipedia, "Root", wurl("Root"), "Root", limits());
    p2.pages = {queued("Root"), queued("Alpha")};
    p2.pages[0].state = PackPageState::Done;
    p2.pages[0].article = article("Root", "<p>x</p>", wurl("Root"));
    Pack o2 = fetchPackPages(p2, {}, {});
    CHECK_EQ(api2.calls.size(), 1u);
    CHECK(o2.pages[0].state == PackPageState::Done && o2.pages[1].state == PackPageState::Done);

    // Every page fails: phase Failed, no throw.
    FakeApi api3;
    api3.handler = [](const std::string&, int& status) {
        status = 500;
        return json::object();
    };
    api3.install();
    Pack p3 = newPack(PackSourceKind::Wikipedia, "Root", wurl("Root"), "Root", limits());
    p3.pages = {queued("Root"), queued("Alpha")};
    progress.clear();
    Pack o3 = fetchPackPages(p3, {}, [&](const PackProgress& p) { progress.push_back(p); });
    CHECK(std::all_of(o3.pages.begin(), o3.pages.end(), [](const PackPage& p) { return p.state == PackPageState::Failed && !p.error.empty(); }));
    CHECK(progress.back().phase == PackPhase::Failed && progress.back().errors.size() == 2);

    // Cancel during the delay keeps the fetched pages.
    api2.calls.clear();
    api2.install();
    PackLimits slow = limits();
    slow.requestDelayMs = 3000;
    Pack p4 = newPack(PackSourceKind::Wikipedia, "Root", wurl("Root"), "Root", slow);
    p4.pages = {queued("Root"), queued("Alpha"), queued("Beta")};
    bool cancel = false;
    std::thread t([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel = true;
    });
    progress.clear();
    Pack o4 = fetchPackPages(p4, {}, [&](const PackProgress& p) { progress.push_back(p); }, &cancel);
    t.join();
    CHECK_EQ(api2.calls.size(), 1u);
    CHECK(o4.pages[0].state == PackPageState::Done && o4.pages[0].article);
    CHECK(o4.pages[1].state == PackPageState::Queued && o4.pages[2].state == PackPageState::Queued);
    CHECK(progress.back().phase == PackPhase::Cancelled);
}

void testScanWebLinks() {
    std::string body =
        "<p><a href=\"https://blog.example.com/post-2\">Post two</a> <a href=\"/post-3#part\">Post three</a> "
        "<a href=\"https://www.blog.example.com/post-2/\">dup</a> <a href=\"https://other.org/page\">Other</a> "
        "<a href=\"mailto:me@example.com\">mail</a> <a href=\"#top\">top</a> <a href=\"https://blog.example.com/file.pdf\">pdf</a> "
        "<a href=\"https://blog.example.com/post-1#comments\">self</a> <a href=\"https://blog.example.com/post-4?utm_source=x\">Post four</a></p>";
    Article a = article("Post one", body, "https://blog.example.com/post-1");
    a.inputKind = InputKind::Url;
    auto out = scanWebLinks(a, "https://blog.example.com/post-1", limits());
    CHECK(keys(out) == (VS{"https://blog.example.com/post-2", "https://blog.example.com/post-3", "https://blog.example.com/post-4"}));
    CHECK(!out.empty() && out[0].title == "Post two" && out[0].depth == 1 && !out[0].selected &&
          out[0].parentKey == "https://blog.example.com/post-1");
    PackLimits l = limits();
    l.sameDomainOnly = false;
    VS k = keys(scanWebLinks(a, "https://blog.example.com/post-1", l));
    CHECK(std::find(k.begin(), k.end(), "https://other.org/page") != k.end());
    CHECK(std::find(k.begin(), k.end(), "https://blog.example.com/file.pdf") == k.end());
}

} // namespace

int main() {
    testNormalize();
    testBodyLinks();
    testScanDepth1();
    testRedirectFlags();
    testBatchesCapDepth2();
    testDelayAndCancel();
    testFailuresDepth0Retry();
    testFetchPackPages();
    testScanWebLinks();
    setFetchOverride({});
    TEST_MAIN_END();
}
