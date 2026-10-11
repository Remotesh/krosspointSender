// Tests for rewriteLinks and packSlug (port of tests/spider/rewrite.test.ts and
// the marker coverage case of followup.test.ts in the web app).
#include "spider_test_util.h"

#include <algorithm>

using namespace ks;
using namespace ks::spider;
using namespace sptest;

namespace {

using VS = std::vector<std::string>;
const std::string W = "https://en.wikipedia.org/wiki/";
const std::string kArrow = "\xE2\x86\x97";

VS fileNames(const std::vector<PackPage>& pages) {
    VS out;
    for (const auto& p : pages) out.push_back(p.fileName);
    return out;
}

void testWikipedia() {
    const std::string rootHtml =
        "<p id=\"intro\">A <a href=\"" + W + "Heat_engine\">heat engine</a> and <a href=\"" + W + "Heat_engine#History\">its history</a>. "
        "<a href=\"" + W + "James_Watt\">James Watt</a>, <a href=\"" + W + "Broken_page\">broken</a>, "
        "<a href=\"" + W + "Steam_engines\">redirect</a>, <a href=\"//en.m.wikipedia.org/wiki/Heat_engine\">mobile</a>. "
        "<a href=\"#cite_note-1\">[1]</a> <a href=\"#nowhere\">gone</a> <a href=\"" + W + "Steam_engine#intro\">self</a> "
        "<a href=\"" + W + "Help:IPA\">ipa</a> <a href=\"https://en.wikipedia.org/w/index.php?title=X&amp;action=edit&amp;redlink=1\">red</a> "
        "<a href=\"https://en.wikipedia.org/w/index.php?title=Steam_engine&amp;action=edit&amp;section=1\">edit</a> "
        "<a href=\"https://example.com/ref\">source</a> <a href=\"https://de.wikipedia.org/wiki/Dampfmaschine\">de</a> "
        "<a href=\"javascript:alert(1)\">js</a></p><ol><li id=\"cite_note-1\">Ref</li></ol>";
    const std::string heatHtml = "<p><a href=\"" + W + "Steam_engine\">back</a> <a href=\"" + W + "James_Watt\">Watt</a></p>";
    auto pages = [&] {
        return std::vector<PackPage>{wikiPage("Steam engine", rootHtml.c_str()), wikiPage("Heat engine", heatHtml.c_str()),
                                     wikiPage("Broken page", nullptr)};
    };
    clearAliases();
    rememberAlias("en", "Steam engines", "Steam engine");
    std::vector<PackPage> input = pages();
    Rewritten r = rewriteLinks(input, PackSourceKind::Wikipedia);
    CHECK(input[0].fileName.empty() && input[0].article->bodyHtml == rootHtml);   // Inputs not changed.
    CHECK(fileNames(r.pages) == (VS{"steam_engine.xhtml", "heat_engine.xhtml", "broken_page.xhtml"}));

    bool ok = false;
    std::string text;
    auto a = anchors(r.pages[0].article->bodyHtml, &ok, &text);
    CHECK(ok);
    struct Exp {
        std::string text, href, cls;
    };
    std::vector<Exp> expected = {
        {"heat engine", "heat_engine.xhtml", ""},
        {"its history", "heat_engine.xhtml", ""},
        {"James Watt" + kArrow, W + "James_Watt", "ext"},
        {"broken" + kArrow, W + "Broken_page", "ext"},
        {"redirect", "steam_engine.xhtml", ""},
        {"mobile", "heat_engine.xhtml", ""},
        {"[1]", "#cite_note-1", ""},
        {"self", "#intro", ""},
        {"source", "https://example.com/ref", ""},
        {"de", "https://de.wikipedia.org/wiki/Dampfmaschine", ""},
    };
    CHECK_EQ(a.size(), expected.size());
    for (size_t i = 0; i < std::min(a.size(), expected.size()); ++i) {
        bool same = a[i].text == expected[i].text && a[i].href == expected[i].href && a[i].cls == expected[i].cls;
        if (!same) std::fprintf(stderr, "anchor %zu: [%s] [%s] [%s]\n", i, a[i].text.c_str(), a[i].href.c_str(), a[i].cls.c_str());
        CHECK(same);
    }
    // Junk links are unwrapped; their text stays.
    for (const char* t : {"ipa", "gone", "edit", "red", "js"}) CHECK(text.find(t) != std::string::npos);
    CHECK(r.pages[0].article->bodyHtml.find("<span class=\"ext\">" + kArrow + "</span>") != std::string::npos);
    CHECK(r.external.size() == 2);
    if (r.external.size() == 2) {
        CHECK(r.external[0].key == "James Watt" && r.external[0].title == "James Watt" && r.external[0].url == W + "James_Watt" &&
              r.external[0].fromKey == "Steam engine");
        CHECK(r.external[1].key == "Broken page" && r.external[1].url == W + "Broken_page");
    }
    auto back = anchors(r.pages[1].article->bodyHtml);
    CHECK(!back.empty() && back[0].href == "steam_engine.xhtml");
    CHECK(!r.pages[2].article);

    // Idempotent: a second run adds no second marker.
    Rewritten once = rewriteLinks(pages(), PackSourceKind::Wikipedia);
    Rewritten twice = rewriteLinks(once.pages, PackSourceKind::Wikipedia);
    CHECK(twice.pages[0].article->bodyHtml == once.pages[0].article->bodyHtml);
    CHECK(twice.pages[1].article->bodyHtml == once.pages[1].article->bodyHtml);
    CHECK(fileNames(twice.pages) == fileNames(once.pages));

    // Existing names stay; clashes and reserved names get "_N".
    std::vector<PackPage> named = {wikiPage("Index", "<p>i</p>"), wikiPage("Steam engine", "<p>a</p>", "custom.xhtml"),
                                   wikiPage("Steam Engine!", "<p>b</p>"), wikiPage("Steam\xE2\x80\x94" "engine", "<p>c</p>")};
    CHECK(fileNames(rewriteLinks(named, PackSourceKind::Wikipedia).pages) ==
          (VS{"index_2.xhtml", "custom.xhtml", "steam_engine.xhtml", "steam_engine_2.xhtml"}));
}

void testCoverage() {
    std::string html = "<p>";
    for (int i = 0; i < 40; ++i) html += "<a href=\"" + W + "Topic_" + std::to_string(i) + "\">Topic_" + std::to_string(i) + "</a> ";
    html += "<a href=\"https://books.google.com/books?id=abc\">book</a> <a href=\"https://archive.org/details/x\">archive</a> "
            "<a href=\"https://en.m.wikipedia.org/wiki/Mobile_topic\">m</a> <a href=\"" + W + "Heat_engine\">bundled</a></p>";
    Rewritten r = rewriteLinks({wikiPage("Steam engine", html.c_str()), wikiPage("Heat engine", "<p>h</p>")}, PackSourceKind::Wikipedia);
    auto a = anchors(r.pages[0].article->bodyHtml);
    int wiki = 0, marked = 0;
    for (const auto& x : a) {
        if (x.href.find("wikipedia.org/wiki/") != std::string::npos) {
            ++wiki;
            if (x.cls == "ext" && x.text.size() >= 3 && x.text.compare(x.text.size() - 3, 3, kArrow) == 0) ++marked;
        }
        if (x.href.find("books.google") != std::string::npos || x.href.find("archive.org") != std::string::npos)
            CHECK(x.cls.empty() && x.text.find(kArrow) == std::string::npos);
        if (x.href == "heat_engine.xhtml") CHECK(x.cls.empty());
    }
    CHECK_EQ(wiki, 41);
    CHECK_EQ(marked, 41);
    CHECK_EQ(r.external.size(), 41u);
    Rewritten again = rewriteLinks(r.pages, PackSourceKind::Wikipedia);
    CHECK(again.pages[0].article->bodyHtml == r.pages[0].article->bodyHtml);
    CHECK_EQ(again.external.size(), 41u);
}

void testWeb() {
    PackPage p1;
    p1.candidate = cand("https://blog.example.com/post-1", "https://blog.example.com/post-1", 0);
    p1.state = PackPageState::Done;
    p1.article = article("Post one",
                         "<p><a href=\"https://blog.example.com/post-2/#c\">two</a> <a href=\"https://blog.example.com/post-3\">three</a> "
                         "<a href=\"https://blog.example.com/post-3\">three again</a> <a href=\"mailto:a@b.c\">mail</a></p>",
                         "https://blog.example.com/post-1");
    p1.article->inputKind = InputKind::Url;
    PackPage p2;
    p2.candidate = cand("https://blog.example.com/post-2", "https://blog.example.com/post-2");
    p2.state = PackPageState::Done;
    p2.article = article("Post two", "<p><a href=\"https://blog.example.com/post-3\">3</a></p>", "https://blog.example.com/post-2");
    Rewritten r = rewriteLinks({p1, p2}, PackSourceKind::Web);
    auto a = anchors(r.pages[0].article->bodyHtml);
    VS hrefs;
    for (const auto& x : a) hrefs.push_back(x.href);
    CHECK(hrefs == (VS{"post_two.xhtml", "https://blog.example.com/post-3", "https://blog.example.com/post-3", "mailto:a@b.c"}));
    CHECK(a.size() == 4 && a[1].cls == "ext" && a[3].cls.empty());
    CHECK(r.external.size() == 1 && r.external[0].key == "https://blog.example.com/post-3" && r.external[0].title == "three" &&
          r.external[0].fromKey == "https://blog.example.com/post-1");
}

void testSlug() {
    CHECK_EQ(packSlug("Steam engine"), std::string("steam_engine"));
    CHECK_EQ(packSlug("Caf\xC3\xA9 M\xC3\xBCller \xE2\x80\x94 \xC3\x86r\xC3\xB8sk\xC3\xB8" "bing"), std::string("cafe_muller_aeroskobing"));
    CHECK_EQ(packSlug("Stra\xC3\x9F" "e in \xC5\x81\xC3\xB3" "d\xC5\xBA"), std::string("strasse_in_lodz"));
    CHECK_EQ(packSlug("  __Hello, World!__ "), std::string("hello_world"));
    CHECK_EQ(packSlug("\xC5\x92uvre \xC3\xA0 \xC3\x9Eingvellir"), std::string("oeuvre_a_thingvellir"));
    CHECK_EQ(packSlug("C++ (programming language)"), std::string("c_programming_language"));
    CHECK_EQ(packSlug("e\xCC\x81t\xC3\xA9"), std::string("ete"));   // A combining mark is dropped.
    CHECK_EQ(packSlug(""), std::string("page"));
    CHECK_EQ(packSlug("***"), std::string("page"));
    CHECK_EQ(packSlug("\xE6\x9D\xB1\xE4\xBA\xAC"), std::string("page"));
    CHECK_EQ(packSlug(std::string(47, 'a') + " bcdef"), std::string(47, 'a'));
    CHECK_EQ(packSlug(std::string(100, 'x')).size(), 48u);
    CHECK_EQ(canonicalTitle("steam_engine"), std::string("Steam engine"));
    CHECK_EQ(canonicalTitle("\xC3\xA9t\xC3\xA9"), std::string("\xC3\x89t\xC3\xA9"));
    CHECK_EQ(webKey("https://WWW.Example.com/a/?utm_source=x&b=1#f"), std::string("https://www.example.com/a?b=1"));
    CHECK_EQ(webKey("mailto:a@b.c"), std::string(""));
}

} // namespace

int main() {
    testWikipedia();
    testCoverage();
    testWeb();
    testSlug();
    TEST_MAIN_END();
}
