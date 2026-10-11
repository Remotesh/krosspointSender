// K-rosspointSender desktop: spider module, internal helpers.
// Only files in src/spider include this header.
// The HTML walks use the libxml2 DOM helpers of the extract module.
#pragma once

#include "spider/spider.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace ks::spider::detail {

// ------------------------------------------------------------- link rules (link_filter.cpp)

// Containers whose links are not body links.
extern const char* const kNonBodySelector;

bool hasNonArticleNamespace(const std::string& title);

struct WikiHref {
    enum Type { Article, Fragment, Junk, Other } type = Junk;
    std::string lang;      // Article only
    std::string title;     // Article only, canonical
    std::string fragment;  // Article and Fragment, decoded, without "#"
};
// Classify a Wikipedia href. pageLang resolves relative hrefs.
WikiHref classifyWikiHref(const std::string& href, const std::string& pageLang);

bool isNonPageUrl(const std::string& url);
std::string siteHost(const std::string& url);     // lower case, without "www."
std::string resolveAlias(const std::string& lang, const std::string& title);
std::string safeDecode(const std::string& s);     // percent-decode; raw text when not valid UTF-8

// ------------------------------------------------------------- HTTP (api.cpp)

// Throw CancelledError when *cancel is true.
void throwIfCancelled(const bool* cancel);

// Wait ms milliseconds. Throws CancelledError as soon as *cancel is true.
void sleepMs(int ms, const bool* cancel);

// Keeps a minimum delay between requests. The first wait() does not wait.
class Pacer {
public:
    Pacer(int delayMs, const bool* cancel) : delayMs_(delayMs), cancel_(cancel) {}
    void wait();

private:
    int delayMs_;
    const bool* cancel_;
    bool first_ = true;
};

using OnWait = std::function<void(int seconds)>;

// GET through the override or libcurl, with the 429/503 retry.
// Throws CancelledError when *cancel becomes true. Never throws for HTTP errors.
extract::FetchResult httpGet(const std::string& url, const bool* cancel, const OnWait& onWait, bool retry);

// Call the MediaWiki API of one language. Adds format=json and formatversion=2.
// Throws std::runtime_error with a plain-word message on HTTP, JSON or API errors.
using Params = std::vector<std::pair<std::string, std::string>>;
nlohmann::json wikiApi(const std::string& lang, const Params& params, const bool* cancel, const OnWait& onWait);

// action=parse of one article, wrapped like extract::fetchWikipedia does it
// (<h1 id="firstHeading"> + <div id="mw-content-text">).
struct WikiPage {
    std::string html;
    std::string finalUrl;
};
WikiPage fetchWikiPage(const std::string& url, const bool* cancel, const OnWait& onWait);

// ------------------------------------------------------------- DOM (rewrite.cpp)

// Remove the images that are not in the first `max`, and figures without an image.
Article limitImages(const Article& a, int max);

} // namespace ks::spider::detail
