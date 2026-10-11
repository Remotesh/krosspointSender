// K-rosspointSender desktop: spider module, HTTP and MediaWiki API.
// Port of wikiApi() in src/spider/wikipediaSpider.ts and of the pacing in
// src/spider/crawlQueue.ts. The real requests use libcurl through
// extract::fetchUrlRetry. Tests can replace the HTTP layer (setFetchOverride).
#include "spider/internal.h"
#include "extract/internal.h"

#include <chrono>
#include <mutex>
#include <thread>

namespace ks::spider {

namespace ex = ks::extract::detail;

namespace {

std::mutex gOverrideMu;
FetchOverride gOverride;

FetchOverride currentOverride() {
    std::lock_guard<std::mutex> lock(gOverrideMu);
    return gOverride;
}

bool isSet(const bool* cancel) {
    // The UI thread writes the flag. A volatile read sees the new value.
    return cancel && *static_cast<const volatile bool*>(cancel);
}

} // namespace

void setFetchOverride(FetchOverride fn) {
    std::lock_guard<std::mutex> lock(gOverrideMu);
    gOverride = std::move(fn);
}

namespace detail {

void throwIfCancelled(const bool* cancel) {
    if (isSet(cancel)) throw CancelledError();
}

void sleepMs(int ms, const bool* cancel) {
    throwIfCancelled(cancel);
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms > 0 ? ms : 0);
    while (std::chrono::steady_clock::now() < until) {
        throwIfCancelled(cancel);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throwIfCancelled(cancel);
}

void Pacer::wait() {
    throwIfCancelled(cancel_);
    if (first_) {
        first_ = false;
        return;
    }
    if (delayMs_ > 0) sleepMs(delayMs_, cancel_);
}

extract::FetchResult httpGet(const std::string& url, const bool* cancel, const OnWait& onWait, bool retry) {
    throwIfCancelled(cancel);
    FetchOverride ov = currentOverride();
    if (!ov) {
        std::function<bool()> fn = [cancel] { return isSet(cancel); };
        extract::FetchResult r = retry ? extract::fetchUrlRetry(url, &fn, 20, onWait) : extract::fetchUrl(url, &fn, 20);
        throwIfCancelled(cancel);
        return r;
    }
    // Test path: the same retry rules, without a Retry-After header.
    for (int attempt = 0;; ++attempt) {
        extract::FetchResult r = ov(url);
        throwIfCancelled(cancel);
        if (!retry || (r.status != 429 && r.status != 503) || attempt >= 3) return r;
        int wait = 1 << (attempt + 1);
        if (onWait) onWait(wait);
        sleepMs(wait * 1000, cancel);
    }
}

nlohmann::json wikiApi(const std::string& lang, const Params& params, const bool* cancel, const OnWait& onWait) {
    std::string url = "https://" + lang + ".wikipedia.org/w/api.php?";
    for (const auto& [k, v] : params) url += ex::encodeUriComponent(k) + "=" + ex::encodeUriComponent(v) + "&";
    url += "format=json&formatversion=2";
    extract::FetchResult r = httpGet(url, cancel, onWait, true);
    if (!r.ok) {
        if (r.status) throw std::runtime_error("Wikipedia answered with HTTP " + std::to_string(r.status) + ".");
        throw std::runtime_error("The request to Wikipedia failed: " + r.error);
    }
    nlohmann::json data = nlohmann::json::parse(r.body, nullptr, false);
    if (data.is_discarded() || !data.is_object()) throw std::runtime_error("Wikipedia sent a response that is not JSON.");
    if (data.contains("error")) {
        const auto& e = data["error"];
        std::string info = "unknown";
        if (e.is_object() && e.contains("info") && e["info"].is_string() && !e["info"].get<std::string>().empty())
            info = e["info"].get<std::string>();
        else if (e.is_object() && e.contains("code") && e["code"].is_string())
            info = e["code"].get<std::string>();
        throw std::runtime_error("Wikipedia API error: " + info);
    }
    return data;
}

WikiPage fetchWikiPage(const std::string& url, const bool* cancel, const OnWait& onWait) {
    auto ref = extract::parseWikipediaUrl(url);
    if (!ref) throw std::runtime_error("This is not a Wikipedia article URL: " + url);
    nlohmann::json data;
    try {
        data = wikiApi(ref->lang,
                       {{"action", "parse"}, {"page", ref->title}, {"prop", "text|displaytitle"}, {"redirects", "1"},
                        {"disableeditsection", "1"}},
                       cancel, onWait);
    } catch (const CancelledError&) {
        throw;
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        const std::string api = "Wikipedia API error: ";
        if (msg.rfind(api, 0) == 0) throw std::runtime_error("Wikipedia could not find this article: " + msg.substr(api.size()));
        throw;
    }
    const nlohmann::json* parse = data.contains("parse") && data["parse"].is_object() ? &data["parse"] : nullptr;
    if (!parse || !parse->contains("text") || !(*parse)["text"].is_string())
        throw std::runtime_error("Wikipedia could not find this article: No article text in the API response.");
    std::string title = parse->contains("title") && (*parse)["title"].is_string() ? (*parse)["title"].get<std::string>() : ref->title;
    std::string underscored = title;
    for (char& c : underscored)
        if (c == ' ') c = '_';
    WikiPage p;
    p.finalUrl = "https://" + ref->host + "/wiki/" + ex::encodeUriComponent(underscored);
    std::string lang = ex::escapeHtml(ref->lang);
    p.html = "<!DOCTYPE html><html lang=\"" + lang + "\"><head><meta charset=\"utf-8\"><title>" + ex::escapeHtml(title) +
             " - Wikipedia</title></head><body><h1 id=\"firstHeading\">" + ex::escapeHtml(title) +
             "</h1><div id=\"mw-content-text\" lang=\"" + lang + "\">" + (*parse)["text"].get<std::string>() + "</div></body></html>";
    return p;
}

} // namespace detail

} // namespace ks::spider
