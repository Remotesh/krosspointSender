// K-rosspointSender desktop: spider module, pack creation, page fetch, web spider.
// Port of src/spider/pack.ts and src/spider/webSpider.ts.
#include "spider/internal.h"
#include "extract/internal.h"

#include <algorithm>
#include <regex>
#include <set>

namespace ks::spider {

namespace ex = ks::extract::detail;
using namespace detail;

namespace {

Article fetchOne(const Pack& pack, const PackPage& page, const ExtractOptions& opts, const bool* cancel, const OnWait& onWait) {
    const std::string& url = page.candidate.url;
    Article article;
    if (pack.kind == PackSourceKind::Wikipedia) {
        WikiPage wp = fetchWikiPage(url, cancel, onWait);
        article = extract::extractWikipedia(wp.html, wp.finalUrl, opts);
        static const std::regex langRe(R"(^https?://([a-z-]+)\.)", std::regex::icase);
        std::smatch m;
        std::string lang = std::regex_search(wp.finalUrl, m, langRe) ? toLower(m[1].str()) : "en";
        if (canonicalTitle(article.title) != page.candidate.key) rememberAlias(lang, article.title, page.candidate.key);
    } else {
        extract::FetchResult r = httpGet(url, cancel, {}, false);
        if (!r.ok) throw std::runtime_error(r.error.empty() ? "The page could not be fetched." : r.error);
        RawInput in;
        in.kind = InputKind::Url;
        in.content = r.body;
        in.sourceUrl = r.finalUrl.empty() ? url : r.finalUrl;
        article = extract::extractArticle(in, opts);
    }
    return limitImages(article, pack.limits.maxImagesPerArticle);
}

} // namespace

Pack newPack(PackSourceKind kind, const std::string& rootKey, const std::string& rootUrl, const std::string& title,
             const PackLimits& limits) {
    Pack p;
    p.id = newUuid();
    p.kind = kind;
    p.title = title;
    p.rootKey = rootKey;
    p.rootUrl = rootUrl;
    p.limits = limits;
    p.createdAt = nowIso();
    p.updatedAt = p.createdAt;
    return p;
}

void fillPagesFromCandidates(Pack& pack, const std::vector<PackCandidate>& cands) {
    pack.pages.clear();
    for (const auto& c : cands) {
        PackPage pg;
        pg.candidate = c;
        pg.state = PackPageState::Queued;
        pack.pages.push_back(std::move(pg));
    }
    pack.updatedAt = nowIso();
}

Pack fetchPackPages(Pack pack, const ExtractOptions& extractOpts, const PackProgressFn& onProgress, const bool* cancel) {
    // Root first.
    auto rootIt = std::find_if(pack.pages.begin(), pack.pages.end(), [&](const PackPage& p) { return p.candidate.key == pack.rootKey; });
    if (rootIt != pack.pages.end() && rootIt != pack.pages.begin()) std::rotate(pack.pages.begin(), rootIt, rootIt + 1);

    ExtractOptions opts = extractOpts;
    opts.keepInfobox = pack.limits.includeInfobox;
    opts.keepReferences = pack.limits.includeReferences;

    std::vector<size_t> work;
    for (size_t i = 0; i < pack.pages.size(); ++i) {
        PackPage& p = pack.pages[i];
        if (!p.candidate.selected) p.state = PackPageState::Skipped;
        else if (p.state == PackPageState::Done && p.article) continue;
        else {
            p.state = PackPageState::Queued;
            work.push_back(i);
        }
    }

    std::vector<PackError> errors;
    const int total = static_cast<int>(work.size());
    int done = 0;
    auto emit = [&](PackPhase phase, const std::string& current, const std::string& message) {
        if (!onProgress) return;
        PackProgress p;
        p.phase = phase;
        p.done = done;
        p.total = total;
        p.currentTitle = current;
        p.message = message;
        p.errors = errors;
        try {
            onProgress(p);
        } catch (...) {
            // A failing listener must not stop the fetch.
        }
    };
    Pacer pacer(pack.limits.requestDelayMs, cancel);

    for (size_t idx : work) {
        PackPage& page = pack.pages[idx];
        const std::string title = page.candidate.title;
        page.state = PackPageState::Fetching;
        page.error.clear();
        emit(PackPhase::Fetching, title, "");
        try {
            pacer.wait();
            page.article = fetchOne(pack, page, opts, cancel,
                                    [&](int s) { emit(PackPhase::Fetching, title, "rate limited, waiting " + std::to_string(s) + "s"); });
            page.state = PackPageState::Done;
        } catch (const CancelledError&) {
            page.state = PackPageState::Queued;
            emit(PackPhase::Cancelled, title, "Cancelled.");
            pack.updatedAt = nowIso();
            return pack;
        } catch (const std::exception& e) {
            page.state = PackPageState::Failed;
            page.error = e.what();
            page.article.reset();
            errors.push_back({page.candidate.key, title, page.error});
        }
        ++done;
        emit(PackPhase::Fetching, title, "");
    }

    const bool failedAll = total > 0 && static_cast<int>(errors.size()) == total;
    emit(failedAll ? PackPhase::Failed : PackPhase::Complete, "",
         failedAll ? "No page could be fetched."
                   : "Fetched " + std::to_string(total - static_cast<int>(errors.size())) + " of " + std::to_string(total) + " pages.");
    pack.updatedAt = nowIso();
    return pack;
}

std::vector<PackCandidate> scanWebLinks(const Article& a, const std::string& baseUrl, const PackLimits& limits) {
    const std::string base = !baseUrl.empty() ? baseUrl : a.sourceUrl;
    const std::string selfKey = base.empty() ? "" : webKey(base);
    const std::string host = siteHost(base);
    const size_t cap = static_cast<size_t>(std::max(1, limits.maxArticles * 5));
    auto loose = [](const std::string& k) {
        static const std::regex www(R"(^(https?://)www\.)", std::regex::icase);
        return std::regex_replace(k, www, "$1");
    };
    const std::string selfLoose = selfKey.empty() ? "" : loose(selfKey);
    auto dom = ex::parseFragment(a.bodyHtml);
    std::set<std::string> seen;
    std::vector<PackCandidate> out;
    for (ex::Node el : ex::querySelectorAll(dom->body(), "a[href]")) {
        if (out.size() >= cap) break;
        std::string href = trim(ex::attr(el, "href"));
        if (href.empty() || href[0] == '#') continue;
        std::string key = webKey(href, base);
        if (key.empty() || key == selfKey || isNonPageUrl(key)) continue;
        // "www." and no "www." are the same page for the duplicate check.
        std::string lk = loose(key);
        if (seen.count(lk) || (!selfLoose.empty() && lk == selfLoose)) continue;
        if (limits.sameDomainOnly && siteHost(key) != host) continue;
        seen.insert(lk);
        std::string text = ex::normText(ex::textContent(el));
        PackCandidate c;
        c.key = key;
        c.title = text.empty() ? key : text;
        c.url = key;
        c.depth = 1;
        c.linkCount = 1;
        c.selected = false;
        c.parentKey = selfKey;
        out.push_back(std::move(c));
    }
    return out;
}

} // namespace ks::spider
