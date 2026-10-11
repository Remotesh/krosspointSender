// Remote folders for the folder browser of the devices window.
// CrossPoint HTTP: GET /api/files?path=<dir> gives a JSON array of
// {name, size, isDirectory}; POST /mkdir with the form fields name and path.
// WebDAV: PROPFIND with Depth 1 gives a multistatus XML body; MKCOL makes a
// folder (405 means it exists). Other methods cannot list folders.
#include "net/net_internal.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace ks::net {

using namespace detail;

namespace {

TransferError unsupported(TransferMethod m) {
    return makeError(TransferErrorCode::Unsupported,
                     "The method " + ks::toString(m) + " cannot list folders on the device. Type the folder path.");
}

// A folder name must be one path segment without reserved characters.
std::string checkName(const std::string& name) {
    std::string n = trim(name);
    bool bad = n.empty() || n == "." || n == "..";
    for (unsigned char c : n)
        if (c < 0x20 || std::strchr("/\\:*?\"<>|", c)) bad = true;
    if (bad) throw makeError(TransferErrorCode::Device, "\"" + name + "\" is not a valid folder name.");
    return n;
}

std::string percentDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// The path part of an href ("http://h/Books/" or "/Books/"), decoded,
// without the trailing slash ("/" for the root).
std::string hrefPath(const std::string& href) {
    std::string h = trim(href);
    size_t scheme = h.find("://");
    if (scheme != std::string::npos) {
        size_t slash = h.find('/', scheme + 3);
        h = slash == std::string::npos ? "/" : h.substr(slash);
    }
    size_t q = h.find_first_of("?#");
    if (q != std::string::npos) h = h.substr(0, q);
    return folderNoTrailingSlash(percentDecode(h));
}

bool isName(const xmlNode* n, const char* local) {
    return n->type == XML_ELEMENT_NODE && n->name && std::strcmp(reinterpret_cast<const char*>(n->name), local) == 0;
}

// The first element with this local name under n (depth first), or null.
const xmlNode* findChild(const xmlNode* n, const char* local) {
    for (const xmlNode* c = n->children; c; c = c->next) {
        if (isName(c, local)) return c;
        if (const xmlNode* d = findChild(c, local)) return d;
    }
    return nullptr;
}

std::string textOf(const xmlNode* n) {
    if (!n) return {};
    xmlChar* t = xmlNodeGetContent(n);
    std::string s = t ? reinterpret_cast<const char*>(t) : "";
    if (t) xmlFree(t);
    return trim(s);
}

void collectResponses(const xmlNode* n, std::vector<const xmlNode*>& out) {
    for (const xmlNode* c = n; c; c = c->next) {
        if (isName(c, "response")) out.push_back(c);
        else if (c->children) collectResponses(c->children, out);
    }
}

void sortEntries(std::vector<RemoteEntry>& v) {
    std::sort(v.begin(), v.end(), [](const RemoteEntry& a, const RemoteEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return toLower(a.name) < toLower(b.name);
    });
}

std::vector<RemoteEntry> listCrossPoint(const DeviceProfile& p, const std::string& path) {
    const std::string dir = folderNoTrailingSlash(path);
    HttpRequest q;
    q.url = baseUrl(p) + "/api/files?path=" + encodeComponent(dir);
    auto r = httpRequest(q, p.host);
    if (r.status < 200 || r.status >= 300) throw httpError(r.status, "Listing " + dir);
    auto j = nlohmann::json::parse(r.body, nullptr, false);
    if (j.is_discarded() || !j.is_array())
        throw NetError(TransferErrorCode::Device, "The device returned a file list that is not a JSON array.");
    std::vector<RemoteEntry> out;
    for (const auto& e : j) {
        if (!e.is_object() || !e.contains("name") || !e["name"].is_string()) continue;
        RemoteEntry re;
        re.name = e["name"].get<std::string>();
        re.isDirectory = e.value("isDirectory", false);
        if (e.contains("size") && e["size"].is_number()) re.size = e["size"].get<uint64_t>();
        if (!re.name.empty()) out.push_back(re);
    }
    return out;
}

std::vector<RemoteEntry> listWebDav(const DeviceProfile& p, const std::string& path) {
    const std::string dir = normalizeFolder(path);
    static const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/><D:getcontentlength/></D:prop></D:propfind>";
    HttpRequest q;
    q.method = "PROPFIND";
    q.url = baseUrl(p) + encodePath(dir);
    q.username = p.username;
    q.password = p.password;
    q.body = &body;
    q.headers = {"Depth: 1", "Content-Type: application/xml; charset=utf-8"};
    auto r = httpRequest(q, p.host);
    if (r.status != 207 && (r.status < 200 || r.status >= 300)) throw httpError(r.status, "Listing " + dir);
    return parsePropfind(r.body, dir);
}

} // namespace

bool canBrowseRemote(TransferMethod m) { return m == TransferMethod::CrossPointHttp || m == TransferMethod::WebDav; }

std::vector<RemoteEntry> parsePropfind(const std::string& xml, const std::string& requestPath) {
    std::vector<RemoteEntry> out;
    xmlDocPtr doc = xmlReadMemory(xml.data(), static_cast<int>(xml.size()), "propfind.xml", nullptr,
                                  XML_PARSE_NONET | XML_PARSE_RECOVER | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return out;
    const std::string self = folderNoTrailingSlash(requestPath);
    std::vector<const xmlNode*> responses;
    collectResponses(xmlDocGetRootElement(doc), responses);
    for (const xmlNode* resp : responses) {
        std::string path = hrefPath(textOf(findChild(resp, "href")));
        if (path.empty() || path == self) continue;
        RemoteEntry e;
        e.name = path.substr(path.find_last_of('/') + 1);
        e.isDirectory = findChild(resp, "collection") != nullptr;
        std::string len = textOf(findChild(resp, "getcontentlength"));
        if (!len.empty() && len.find_first_not_of("0123456789") == std::string::npos) e.size = std::stoull(len);
        if (!e.name.empty()) out.push_back(e);
    }
    xmlFreeDoc(doc);
    return out;
}

std::vector<RemoteEntry> listRemoteFolder(const DeviceProfile& p, const std::string& path) {
    std::vector<RemoteEntry> out;
    try {
        if (p.method == TransferMethod::CrossPointHttp) out = listCrossPoint(p, path);
        else if (p.method == TransferMethod::WebDav) out = listWebDav(p, path);
        else throw unsupported(p.method);
    } catch (const NetError& e) {
        throw e.err;
    }
    sortEntries(out);
    return out;
}

void makeRemoteFolder(const DeviceProfile& p, const std::string& path, const std::string& name) {
    const std::string n = checkName(name);
    try {
        if (p.method == TransferMethod::CrossPointHttp) {
            std::string form = formEncode({{"name", n}, {"path", folderNoTrailingSlash(path)}});
            HttpRequest q;
            q.method = "POST";
            q.url = baseUrl(p) + "/mkdir";
            q.body = &form;
            q.headers.push_back("Content-Type: application/x-www-form-urlencoded");
            auto r = httpRequest(q, p.host);
            if (r.status < 200 || r.status >= 300) throw httpError(r.status, "Creating the folder " + n, r.body);
            return;
        }
        if (p.method == TransferMethod::WebDav) {
            const std::string target = normalizeFolder(path) + n + "/";
            HttpRequest q;
            q.method = "MKCOL";
            q.url = baseUrl(p) + encodePath(target);
            q.username = p.username;
            q.password = p.password;
            auto r = httpRequest(q, p.host);
            if ((r.status >= 200 && r.status < 300) || r.status == 405) return;   // 405: the folder exists
            throw httpError(r.status, "Creating the folder " + target, r.body);
        }
        throw unsupported(p.method);
    } catch (const NetError& e) {
        throw e.err;
    }
}

} // namespace ks::net
