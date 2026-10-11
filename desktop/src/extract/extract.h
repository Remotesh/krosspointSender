// k-rosspointSender desktop: extract module (public API).
// This module turns raw input (URL fetch, HTML, Markdown, text, bookmarklet
// payload) into a clean Article. The web app (src/extractors/*.ts) is the
// reference behaviour. See docs/DESKTOP.md, section "extract/".
#pragma once

#include "core/types.h"

#include <functional>
#include <optional>
#include <string>

namespace ks::extract {

// ------------------------------------------------------------- URLs and input kind

// Clean a user-typed URL. Trims, adds https://, rejects non-http(s),
// removes tracking parameters (utm_*, fbclid, ...), lowercases the host.
// Returns nullopt when the input is not a web URL.
std::optional<std::string> normalizeUrl(const std::string& in);

// True for a *.wikipedia.org article URL (/wiki/... or /w/index.php).
bool isWikipediaUrl(const std::string& url);

// Guess the kind of the given text. A known file extension wins.
InputKind detectInputKind(const std::string& text, const std::string& fileName = "");

// ------------------------------------------------------------- network

struct FetchResult {
    bool ok = false;
    int status = 0;            // HTTP status, 0 when no answer
    std::string body;
    std::string finalUrl;      // URL after redirects
    std::string contentType;
    std::string error;         // plain-word problem text when ok is false
};

// libcurl GET. Follows redirects. User agent "k-rosspointSender/1.0".
// `cancelled` is polled during the transfer. Never throws.
FetchResult fetchUrl(const std::string& url, const std::function<bool()>* cancelled = nullptr, int timeoutSec = 20);

// libcurl GET with a retry on HTTP 429 and 503 (port of fetchWithRetry).
// The wait is the Retry-After value (max 60 s), else 2 s, 4 s, 8 s.
// Max 3 retries. After the last retry the 429/503 result comes back.
// onRetrySeconds (can be empty) gets each wait before it starts.
// A cancel during a wait gives ok=false and the error "Cancelled.".
// fetchWikipedia uses this helper. Never throws.
FetchResult fetchUrlRetry(const std::string& url, const std::function<bool()>* cancelled, int timeoutSec,
                          const std::function<void(int)>& onRetrySeconds);

// Fetch a Wikipedia article through the MediaWiki action=parse API. The body
// is a small HTML document with <h1 id="firstHeading"> and
// <div id="mw-content-text">. finalUrl is the canonical /wiki/ URL.
FetchResult fetchWikipedia(const std::string& url);

struct WikiRef {
    std::string lang;   // "en"
    std::string title;  // "E Ink" (decoded, spaces)
    std::string host;   // "en.wikipedia.org"
};
std::optional<WikiRef> parseWikipediaUrl(const std::string& url);

// Download the images of an article. Best effort: failed images are missing
// from the map. data: URLs are decoded. Never throws.
ImageMap fetchImages(const Article& a, const std::function<bool()>* cancelled = nullptr);

// ------------------------------------------------------------- extraction

// Turn any raw input into a clean Article. Dispatches on in.kind.
// url/html/bookmarklet input with a Wikipedia sourceUrl uses the Wikipedia path.
// Throws std::runtime_error when the input is empty or the bookmarklet payload is bad.
Article extractArticle(const RawInput& in, const ExtractOptions& opts = {});

// Clean a Wikipedia page (desktop page, mobile page, or fetchWikipedia output).
Article extractWikipedia(const std::string& html, const std::string& url, const ExtractOptions& opts = {});

// Allowlist sanitizer. Removes page chrome, linearizes the layout, keeps only
// the allowed tags and attributes, resolves relative URLs against baseUrl.
// Returns a well-formed XHTML fragment (void tags self-closed, text escaped).
std::string sanitizeHtml(const std::string& html, const ExtractOptions& opts, const std::string& baseUrl);

// Markdown (GitHub flavour, md4c) to sanitized XHTML.
std::string markdownToHtml(const std::string& md);

// Plain text to XHTML paragraphs. A blank line starts a paragraph.
// Hard-wrapped prose joins into one line. Short lines keep <br/>.
std::string textToHtml(const std::string& txt);

// Count whitespace-separated words of the text content.
int countWords(const std::string& html);

// Text content. Block tags become newlines (a blank line between blocks).
std::string htmlToText(const std::string& html);

} // namespace ks::extract
