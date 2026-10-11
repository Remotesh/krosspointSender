// k-rosspointSender desktop: spider module (Spider Pack mode), public API.
// Port of the web src/spider/*.ts. See docs/DESKTOP.md, section
// "Addendum 2026-10-10: web parity", part "spider/".
//
// Cancel rules:
// - scanWikipediaLinks throws CancelledError when *cancel becomes true.
//   It sends a last progress with phase Cancelled first.
// - fetchPackPages does not throw for a cancel. It returns the pack with the
//   pages that it fetched (the current page goes back to Queued) and sends a
//   last progress with phase Cancelled.
// Other failures:
// - scanWikipediaLinks throws std::runtime_error when the root page fails.
//   A failed depth-1 scan or disambiguation batch goes into progress.errors.
// - fetchPackPages records per-page errors and never throws for them.
#pragma once

#include "core/types.h"
#include "extract/extract.h"

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ks::spider {

// The error for a cancelled operation. what() is "Cancelled.".
class CancelledError : public std::runtime_error {
public:
    CancelledError() : std::runtime_error("Cancelled.") {}
};

// ------------------------------------------------------------- Wikipedia spider

struct WikiRoot {
    std::string lang;   // "en"
    std::string title;  // canonical: "Steam engine"
    std::string url;    // "https://en.wikipedia.org/wiki/Steam_engine"
};

// Read a Wikipedia URL (desktop, mobile, no scheme) or a plain title.
// Returns nullopt for empty input, other sites and non-article namespaces.
std::optional<WikiRoot> normalizeWikipediaTitle(const std::string& input, const std::string& lang = "en");

// Scan the links of a root article, breadth-first to limits.depth.
// Candidate 0 is the root (selected). Depth-1 pages that are not
// disambiguation pages are selected until maxArticles (root included).
// Max maxArticles * 5 candidates. requestDelayMs separates all API calls.
std::vector<PackCandidate> scanWikipediaLinks(const WikiRoot& root, const PackLimits& limits, const PackProgressFn& onProgress,
                                              const bool* cancel = nullptr);

// Body links of one action=parse HTML text, each title once. Body-text links
// first, then links that occur only in figures and captions.
struct LinkMeta {
    int ns = 0;
    std::string title;
    bool exists = true;
};
std::vector<std::string> extractBodyLinks(const std::string& html, const std::string& lang, const std::string& selfTitle,
                                          const std::vector<LinkMeta>& linksMeta = {});

// ------------------------------------------------------------- packs

// An empty pack with a new id and timestamps. limits are copied.
Pack newPack(PackSourceKind kind, const std::string& rootKey, const std::string& rootUrl, const std::string& title,
             const PackLimits& limits);

// One PackPage per candidate (state Queued). Existing pages are replaced.
void fillPagesFromCandidates(Pack& pack, const std::vector<PackCandidate>& cands);

// Fetch the pages of a pack, one at a time, root first. See the cancel rules above.
Pack fetchPackPages(Pack pack, const ExtractOptions& opts, const PackProgressFn& onProgress, const bool* cancel = nullptr);

// Candidates from the links of one cleaned article (web packs). Depth 1, not selected.
std::vector<PackCandidate> scanWebLinks(const Article& a, const std::string& baseUrl, const PackLimits& limits);

// ------------------------------------------------------------- links between pages

struct Rewritten {
    std::vector<PackPage> pages;
    std::vector<PackExternalLink> external;
};

// Rewrite the links of the fetched pages (see the spec). Idempotent.
// Gives each page a unique fileName when it has none. Inputs are not changed.
Rewritten rewriteLinks(const std::vector<PackPage>& pages, PackSourceKind kind);

// File-name slug: lowercase ASCII, Latin accents folded, other runs "_",
// max 48 characters, never empty ("page").
std::string packSlug(const std::string& title);

// ------------------------------------------------------------- small helpers (also used by tests)

// "_" to space, one space, first letter upper case.
std::string canonicalTitle(const std::string& raw);

// Article URL for a title: "https://<lang>.wikipedia.org/wiki/<Title_with_underscores>".
std::string wikipediaArticleUrl(const std::string& lang, const std::string& title);

// Stable key for a web page URL (absolute, no fragment, no tracking parameters,
// no trailing slash). Empty for a URL that is not http(s).
std::string webKey(const std::string& href, const std::string& base = "");

// Redirect aliases that scans and fetches learned (rewriteLinks uses them).
void rememberAlias(const std::string& lang, const std::string& from, const std::string& to);
void clearAliases();

// ------------------------------------------------------------- test hook

// Replace the HTTP layer. Every spider request goes through `fn` when it is
// set (the 429/503 retry still applies, with 2, 4, 8 s waits). Empty = real libcurl.
using FetchOverride = std::function<extract::FetchResult(const std::string& url)>;
void setFetchOverride(FetchOverride fn);

} // namespace ks::spider
