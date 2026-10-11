// k-rosspointSender desktop: extract module, network fetch with libcurl.
// Port of src/extractors/fetch.ts. The desktop has no CORS limit, so a
// plain GET reads any page.
#include "extract/internal.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <thread>

namespace ks::extract {

using namespace detail;

namespace {

const char* kUserAgent = "k-rosspointSender/1.0";
const size_t kMaxBody = 64u * 1024u * 1024u;   // 64 MiB

void globalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct XferCtx {
    std::string* body = nullptr;
    const std::function<bool()>* cancelled = nullptr;
    std::string* retryAfter = nullptr;   // receives the Retry-After header value
    bool tooBig = false;
};

// Keep the Retry-After header. Other headers are not used.
size_t onHeader(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* ctx = static_cast<XferCtx*>(user);
    size_t n = size * nmemb;
    if (ctx->retryAfter) {
        std::string line(ptr, n);
        const std::string key = "retry-after:";
        if (line.size() > key.size() && toLower(line.substr(0, key.size())) == key) *ctx->retryAfter = trim(line.substr(key.size()));
    }
    return n;
}

size_t onWrite(char* ptr, size_t size, size_t nmemb, void* user) {
    auto* ctx = static_cast<XferCtx*>(user);
    size_t n = size * nmemb;
    if (ctx->body->size() + n > kMaxBody) {
        ctx->tooBig = true;
        return 0;
    }
    ctx->body->append(ptr, n);
    return n;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<XferCtx*>(user);
    if (ctx->cancelled && *ctx->cancelled && (*ctx->cancelled)()) return 1;
    return 0;
}

bool isCaError(CURLcode rc) {
    return rc == CURLE_PEER_FAILED_VERIFICATION || rc == CURLE_SSL_CACERT_BADFILE || rc == CURLE_SSL_CERTPROBLEM;
}

FetchResult doGet(const std::string& url, const std::function<bool()>* cancelled, int timeoutSec, const char* accept, bool nativeCa,
                  std::string* retryAfter = nullptr) {
    globalInit();
    FetchResult r;
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "The network library did not start.";
        return r;
    }
    XferCtx ctx;
    ctx.body = &r.body;
    ctx.cancelled = cancelled;
    ctx.retryAfter = retryAfter;
    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_slist* headers = nullptr;
    std::string acceptHeader = std::string("Accept: ") + accept;
    headers = curl_slist_append(headers, acceptHeader.c_str());
    headers = curl_slist_append(headers, "Accept-Language: en;q=0.9, *;q=0.5");

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, static_cast<long>(timeoutSec > 0 ? timeoutSec : 20));
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(c, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &ctx);
#ifdef _WIN32
    if (nativeCa) curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#else
    (void)nativeCa;
#endif

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    r.status = static_cast<int>(status);
    char* eff = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) r.finalUrl = eff;
    if (r.finalUrl.empty()) r.finalUrl = url;
    char* ct = nullptr;
    if (curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) r.contentType = ct;
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (rc != CURLE_OK) {
        r.body.clear();
        if (isCaError(rc) && !nativeCa) {
#ifdef _WIN32
            // The OpenSSL CA bundle is missing or old. Try the Windows store.
            return doGet(url, cancelled, timeoutSec, accept, true, retryAfter);
#endif
        }
        switch (rc) {
            case CURLE_ABORTED_BY_CALLBACK: r.error = "Cancelled."; break;
            case CURLE_OPERATION_TIMEDOUT: r.error = "The site did not answer in " + std::to_string(timeoutSec) + " seconds."; break;
            case CURLE_COULDNT_RESOLVE_HOST: r.error = "The host name cannot be found. Check the address and your connection."; break;
            case CURLE_COULDNT_CONNECT: r.error = "The connection to the site failed."; break;
            case CURLE_UNSUPPORTED_PROTOCOL: r.error = "Only http and https addresses are supported."; break;
            case CURLE_TOO_MANY_REDIRECTS: r.error = "The site sent too many redirects."; break;
            case CURLE_WRITE_ERROR:
                if (ctx.tooBig) {
                    r.error = "The page is too large (more than 64 MiB).";
                    break;
                }
                [[fallthrough]];
            default:
                r.error = std::string("The request failed: ") + (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        }
        return r;
    }
    if (r.status < 200 || r.status > 299) {
        r.error = "The site answered with HTTP " + std::to_string(r.status) + ".";
        if (r.status == 401 || r.status == 403) r.error += " The page can need a login, or it blocks automatic reading.";
        else if (r.status == 404 || r.status == 410) r.error += " Check the address.";
        else if (r.status == 429) r.error += " You sent too many requests. Wait, then try again.";
        return r;
    }
    r.ok = true;
    return r;
}

// When the server names a charset that the page does not declare, add a
// <meta charset> so that the HTML parser decodes the bytes correctly.
void declareCharset(FetchResult& r) {
    std::string ct = toLower(r.contentType);
    if (ct.find("html") == std::string::npos) return;
    size_t p = ct.find("charset=");
    if (p == std::string::npos) return;
    std::string cs = ct.substr(p + 8);
    cs = cs.substr(0, cs.find_first_of("; "));
    if (!cs.empty() && (cs.front() == '"' || cs.front() == '\'')) cs = cs.substr(1);
    if (!cs.empty() && (cs.back() == '"' || cs.back() == '\'')) cs.pop_back();
    if (cs.empty() || cs == "utf-8" || cs == "utf8") return;
    if (toLower(r.body.substr(0, 4096)).find("charset") != std::string::npos) return;
    r.body = "<meta charset=\"" + cs + "\">" + r.body;
}

std::string decodeBase64Loose(const std::string& in) {
    auto val = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    unsigned buf = 0;
    int bits = 0;
    for (unsigned char c : in) {
        int v = val(c);
        if (v < 0) continue;   // Skip whitespace and padding.
        buf = (buf << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string decodeDataUrl(const std::string& url) {
    size_t comma = url.find(',');
    if (comma == std::string::npos) return "";
    std::string meta = toLower(url.substr(5, comma - 5));
    std::string payload = url.substr(comma + 1);
    if (meta.size() >= 7 && meta.compare(meta.size() - 7, 7, ";base64") == 0) return decodeBase64Loose(percentDecode(payload));
    return percentDecode(payload);
}

// Seconds from a Retry-After value (seconds or an HTTP date), max 60.
// Returns -1 when the value is missing or not readable.
int parseRetryAfter(const std::string& value) {
    const int kMaxWait = 60;
    std::string v = trim(value);
    if (v.empty()) return -1;
    bool number = std::all_of(v.begin(), v.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    if (number && v[0] != '.') {
        double d = std::atof(v.c_str());
        return std::min(kMaxWait, static_cast<int>(std::ceil(d)));
    }
    time_t t = curl_getdate(v.c_str(), nullptr);
    if (t == -1) return -1;
    long long diff = static_cast<long long>(t) - static_cast<long long>(std::time(nullptr));
    return static_cast<int>(std::max<long long>(0, std::min<long long>(kMaxWait, diff)));
}

bool isCancelled(const std::function<bool()>* cancelled) { return cancelled && *cancelled && (*cancelled)(); }

} // namespace

FetchResult fetchUrlRetry(const std::string& url, const std::function<bool()>* cancelled, int timeoutSec,
                          const std::function<void(int)>& onRetrySeconds) {
    const int kMaxRetries = 3;
    try {
        for (int attempt = 0;; ++attempt) {
            std::string retryAfter;
            FetchResult r = doGet(url, cancelled, timeoutSec, "application/json,text/html;q=0.9,*/*;q=0.5", false, &retryAfter);
            if ((r.status != 429 && r.status != 503) || attempt >= kMaxRetries) return r;
            int wait = parseRetryAfter(retryAfter);
            if (wait < 0) wait = 1 << (attempt + 1);   // 2, 4, 8 s
            if (onRetrySeconds) onRetrySeconds(wait);
            // Wait in short steps, so that a cancel stops the wait quickly.
            auto until = std::chrono::steady_clock::now() + std::chrono::seconds(wait);
            while (std::chrono::steady_clock::now() < until) {
                if (isCancelled(cancelled)) {
                    FetchResult c;
                    c.error = "Cancelled.";
                    c.finalUrl = url;
                    return c;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    } catch (const std::exception& e) {
        FetchResult r;
        r.error = e.what();
        return r;
    }
}

FetchResult fetchUrl(const std::string& url, const std::function<bool()>* cancelled, int timeoutSec) {
    try {
        FetchResult r = doGet(url, cancelled, timeoutSec, "text/html,application/xhtml+xml;q=0.9,*/*;q=0.5", false);
        if (r.ok) declareCharset(r);
        return r;
    } catch (const std::exception& e) {
        FetchResult r;
        r.error = e.what();
        return r;
    }
}

FetchResult fetchWikipedia(const std::string& url) {
    FetchResult out;
    try {
        auto ref = parseWikipediaUrl(url);
        if (!ref) {
            out.error = "This is not a Wikipedia article URL: " + url;
            return out;
        }
        std::string api = "https://" + ref->host + "/w/api.php?action=parse&page=" + encodeUriComponent(ref->title) +
                          "&prop=text%7Cdisplaytitle&redirects=1&disableeditsection=1&format=json&formatversion=2";
        FetchResult r = fetchUrlRetry(api, nullptr, 20, {});
        if (!r.ok) {
            if (r.status) r.error = "Wikipedia answered with HTTP " + std::to_string(r.status) + ".";
            return r;
        }
        nlohmann::json data = nlohmann::json::parse(r.body, nullptr, false);
        const nlohmann::json* parse = nullptr;
        if (data.is_object() && data.contains("parse") && data["parse"].is_object()) parse = &data["parse"];
        if (data.is_discarded() || !parse || !parse->contains("text") || !(*parse)["text"].is_string() || (data.is_object() && data.contains("error"))) {
            std::string info = "No article text in the API response.";
            if (data.is_object() && data.contains("error") && data["error"].is_object() && data["error"].contains("info") && data["error"]["info"].is_string())
                info = data["error"]["info"].get<std::string>();
            out.status = 404;
            out.finalUrl = url;
            out.error = "Wikipedia could not find this article: " + info;
            return out;
        }
        std::string title = parse->contains("title") && (*parse)["title"].is_string() ? (*parse)["title"].get<std::string>() : ref->title;
        std::string underscored = title;
        for (char& c : underscored)
            if (c == ' ') c = '_';
        out.ok = true;
        out.status = r.status;
        out.contentType = "text/html; charset=utf-8";
        out.finalUrl = "https://" + ref->host + "/wiki/" + encodeUriComponent(underscored);
        std::string lang = escapeHtml(ref->lang);
        out.body = "<!DOCTYPE html><html lang=\"" + lang + "\"><head><meta charset=\"utf-8\"><title>" + escapeHtml(title) +
                   " - Wikipedia</title></head><body><h1 id=\"firstHeading\">" + escapeHtml(title) +
                   "</h1><div id=\"mw-content-text\" lang=\"" + lang + "\">" + (*parse)["text"].get<std::string>() + "</div></body></html>";
        return out;
    } catch (const std::exception& e) {
        out.ok = false;
        out.error = std::string("The Wikipedia response cannot be read: ") + e.what();
        return out;
    }
}

ImageMap fetchImages(const Article& a, const std::function<bool()>* cancelled) {
    ImageMap out;
    try {
        std::vector<std::string> srcs;
        for (const auto& img : a.images)
            if (!img.src.empty() && std::find(srcs.begin(), srcs.end(), img.src) == srcs.end()) srcs.push_back(img.src);
        if (srcs.empty()) return out;
        std::mutex mu;
        std::atomic<size_t> next{0};
        auto worker = [&]() {
            for (;;) {
                if (cancelled && *cancelled && (*cancelled)()) return;
                size_t i = next++;
                if (i >= srcs.size()) return;
                const std::string& src = srcs[i];
                std::string bytes;
                try {
                    if (startsWithNoCase(src, "data:")) {
                        bytes = decodeDataUrl(src);
                    } else if (startsWithNoCase(src, "http://") || startsWithNoCase(src, "https://")) {
                        FetchResult r = doGet(src, cancelled, 20, "image/avif,image/webp,image/png,image/jpeg,image/*;q=0.8,*/*;q=0.5", false);
                        if (r.ok) bytes = std::move(r.body);
                    }
                } catch (...) {
                    bytes.clear();
                }
                if (!bytes.empty()) {
                    std::lock_guard<std::mutex> lock(mu);
                    out[src] = std::move(bytes);
                }
            }
        };
        size_t n = std::min<size_t>(4, srcs.size());
        std::vector<std::thread> threads;
        try {
            for (size_t i = 0; i < n; ++i) threads.emplace_back(worker);
        } catch (...) {
            // No more threads. The running workers finish the list.
        }
        if (threads.empty()) worker();
        for (auto& t : threads) t.join();
    } catch (...) {
        // Best effort: return what we have.
    }
    return out;
}

} // namespace ks::extract
