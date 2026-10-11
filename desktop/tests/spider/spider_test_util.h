// Shared helpers for the spider tests: a fake MediaWiki API and small parsers.
#pragma once

#include "check.h"
#include "core/types.h"
#include "spider/spider.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace sptest {

using json = nlohmann::json;
using ks::extract::FetchResult;

inline ks::PackLimits limits() {
    ks::PackLimits l;
    l.requestDelayMs = 0;
    return l;
}

inline std::string fixture(const std::string& name) {
    bool ok = false;
    std::string s = ks::readFile(std::string(KS_SPIDER_FIXTURES) + "/" + name, &ok);
    if (!ok) std::fprintf(stderr, "missing fixture %s\n", name.c_str());
    return s;
}

inline int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) {
            o += static_cast<char>(hexv(s[i + 1]) * 16 + hexv(s[i + 2]));
            i += 2;
        } else {
            o += s[i] == '+' ? ' ' : s[i];
        }
    }
    return o;
}

// One query parameter of a URL ("" when missing).
inline std::string param(const std::string& url, const std::string& key) {
    size_t q = url.find('?');
    if (q == std::string::npos) return "";
    std::string rest = url.substr(q + 1);
    size_t start = 0;
    while (start <= rest.size()) {
        size_t amp = rest.find('&', start);
        std::string piece = rest.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        size_t eq = piece.find('=');
        if (decode(piece.substr(0, eq)) == key) return eq == std::string::npos ? "" : decode(piece.substr(eq + 1));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return "";
}

inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

struct Call {
    std::string url;
    std::chrono::steady_clock::time_point at;
};

// Install a fake API. The handler gets the URL and returns a JSON body, or a
// FetchResult through `status` (non-200 = HTTP error).
struct FakeApi {
    std::mutex mu;
    std::vector<Call> calls;
    std::function<json(const std::string& url, int& status)> handler;

    void install() {
        ks::spider::setFetchOverride([this](const std::string& url) {
            int status = 200;
            json body;
            {
                std::lock_guard<std::mutex> lock(mu);
                calls.push_back({url, std::chrono::steady_clock::now()});
            }
            body = handler(url, status);
            FetchResult r;
            r.status = status;
            r.ok = status >= 200 && status < 300;
            r.finalUrl = url;
            r.body = body.dump();
            if (!r.ok) r.error = "HTTP " + std::to_string(status);
            return r;
        });
    }
    std::vector<std::string> paramsOf(const std::string& key, const std::string& action = "") {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<std::string> out;
        for (const auto& c : calls)
            if (action.empty() || param(c.url, "action") == action) out.push_back(param(c.url, key));
        return out;
    }
    size_t count(const std::string& action = "") { return paramsOf("action", action).size(); }
};

// A parse response whose body links to the given titles, in order.
inline json parseBody(const std::string& title, const std::vector<std::string>& links) {
    std::string anchors;
    json meta = json::array();
    for (const auto& l : links) {
        std::string u = l;
        for (char& c : u)
            if (c == ' ') c = '_';
        anchors += (anchors.empty() ? "" : ", ") + std::string("<a href=\"/wiki/") + u + "\" title=\"" + l + "\">" + l + "</a>";
        meta.push_back({{"ns", 0}, {"title", l}, {"exists", true}});
    }
    return {{"parse", {{"title", title}, {"text", "<div class=\"mw-parser-output\"><p>" + title + " links to " + anchors + ".</p></div>"}, {"links", meta}}}};
}

// A pageprops response with every title present and no flags.
inline json plainPageProps(const std::string& url) {
    json pages = json::array();
    int i = 1;
    for (const auto& t : split(param(url, "titles"), '|')) pages.push_back({{"pageid", i++}, {"ns", 0}, {"title", t}});
    return {{"query", {{"pages", pages}}}};
}

inline json withRedirect(const std::string& url, const std::string& from, const std::string& to) {
    json j = plainPageProps(url);
    j["query"]["redirects"] = json::array({{{"from", from}, {"to", to}}});
    return j;
}

struct Anchor {
    std::string text, href, cls;
};

static inline void collectAnchors(xmlNodePtr n, std::vector<Anchor>& out, std::string* allText) {
    for (xmlNodePtr c = n->children; c; c = c->next) {
        if (c->type == XML_TEXT_NODE && allText && c->content) *allText += reinterpret_cast<const char*>(c->content);
        if (c->type != XML_ELEMENT_NODE) continue;
        if (xmlStrcmp(c->name, BAD_CAST "a") == 0) {
            Anchor a;
            xmlChar* t = xmlNodeGetContent(c);
            a.text = t ? reinterpret_cast<const char*>(t) : "";
            xmlFree(t);
            xmlChar* h = xmlGetProp(c, BAD_CAST "href");
            a.href = h ? reinterpret_cast<const char*>(h) : "";
            xmlFree(h);
            xmlChar* k = xmlGetProp(c, BAD_CAST "class");
            a.cls = k ? reinterpret_cast<const char*>(k) : "";
            xmlFree(k);
            out.push_back(a);
        }
        collectAnchors(c, out, allText);
    }
}

// Parse an XHTML fragment with the strict XML parser. `ok` is false when it
// is not well-formed. Returns the anchors in document order.
inline std::vector<Anchor> anchors(const std::string& body, bool* ok = nullptr, std::string* allText = nullptr) {
    std::string doc = "<div>" + body + "</div>";
    xmlDocPtr d = xmlReadMemory(doc.data(), static_cast<int>(doc.size()), "f.xml", "UTF-8", XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (ok) *ok = d != nullptr;
    std::vector<Anchor> out;
    if (d) {
        collectAnchors(xmlDocGetRootElement(d), out, allText);
        xmlFreeDoc(d);
    }
    return out;
}

inline ks::PackCandidate cand(const std::string& key, const std::string& url, int depth = 1, bool selected = true) {
    ks::PackCandidate c;
    c.key = key;
    c.title = key;
    c.url = url;
    c.depth = depth;
    c.linkCount = 1;
    c.selected = selected;
    return c;
}

inline std::string wurl(std::string t) {
    for (char& c : t)
        if (c == ' ') c = '_';
    return "https://en.wikipedia.org/wiki/" + t;
}

inline ks::Article article(const std::string& title, const std::string& body, const std::string& url) {
    ks::Article a;
    a.id = "id-" + title;
    a.title = title;
    a.capturedAt = "2026-10-10T00:00:00Z";
    a.sourceUrl = url;
    a.bodyHtml = body;
    a.wordCount = 10;
    a.inputKind = ks::InputKind::Wikipedia;
    return a;
}

// A Done page with an article, or a Failed page when body is null.
inline ks::PackPage wikiPage(const std::string& title, const char* body, const std::string& fileName = "") {
    ks::PackPage p;
    p.candidate = cand(title, wurl(title));
    if (body) {
        p.state = ks::PackPageState::Done;
        p.article = article(title, body, wurl(title));
    } else {
        p.state = ks::PackPageState::Failed;
    }
    p.fileName = fileName;
    return p;
}

} // namespace sptest
