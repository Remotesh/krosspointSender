// Shared helpers for the transports: errors, progress, paths and libcurl.
#include "net/net_internal.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <thread>

namespace ks::net {

std::string toString(TransferErrorCode c) {
    switch (c) {
        case TransferErrorCode::Network: return "network";
        case TransferErrorCode::NotFound: return "not-found";
        case TransferErrorCode::Auth: return "auth";
        case TransferErrorCode::Exists: return "exists";
        case TransferErrorCode::Unsupported: return "unsupported";
        case TransferErrorCode::Cancelled: return "cancelled";
        case TransferErrorCode::Device: return "device";
        case TransferErrorCode::Unknown: return "unknown";
    }
    return "unknown";
}

std::optional<TransferErrorCode> errorCodeFrom(const std::string& s) {
    for (auto c : {TransferErrorCode::Network, TransferErrorCode::NotFound, TransferErrorCode::Auth,
                   TransferErrorCode::Exists, TransferErrorCode::Unsupported, TransferErrorCode::Cancelled,
                   TransferErrorCode::Device, TransferErrorCode::Unknown})
        if (toString(c) == s) return c;
    if (s == "mixed-content") return TransferErrorCode::Network;   // web-only code
    return std::nullopt;
}

std::vector<std::string> folderSegments(const std::string& folder) {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&] {
        std::string t = ks::trim(cur);
        if (!t.empty() && t != "." && t != "..") out.push_back(t);
        cur.clear();
    };
    for (char c : folder) {
        if (c == '/' || c == '\\') flush();
        else cur += c;
    }
    flush();
    return out;
}

std::string normalizeFolder(const std::string& folder) {
    auto segs = folderSegments(folder);
    if (segs.empty()) return "/";
    std::string out = "/";
    for (auto& s : segs) out += s + "/";
    return out;
}

std::string folderNoTrailingSlash(const std::string& folder) {
    std::string n = normalizeFolder(folder);
    if (n.size() > 1) n.pop_back();
    return n;
}

std::string remoteFilePath(const std::string& folder, const std::string& name) {
    return normalizeFolder(folder) + name;
}

uint32_t crc32(const std::string& bytes) {
    static uint32_t table[256];
    static std::once_flag once;
    std::call_once(once, [] {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
    });
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : bytes) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace detail {

TransferError makeError(TransferErrorCode c, const std::string& m) {
    TransferError e;
    e.code = c;
    e.message = m;
    return e;
}

NetError cancelled() { return NetError(TransferErrorCode::Cancelled, "The transfer was cancelled."); }

void sleepCancellable(int ms, const bool* cancel) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (isCancelled(cancel)) throw cancelled();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (isCancelled(cancel)) throw cancelled();
}

void Emitter::operator()(TransferState s, const std::string& msg, double fraction, uint64_t sent,
                         uint64_t total) const {
    if (!fn_) return;
    TransferProgress p;
    p.state = s;
    p.message = msg;
    p.fraction = fraction;
    p.bytesSent = sent;
    p.bytesTotal = total;
    try {
        fn_(p);
    } catch (...) {
        // Ignore errors in the UI callback. They must not stop a transfer.
    }
}

TransferResult runSend(const OutputFile& file, const ProgressFn& onProgress,
                       const std::function<std::string()>& body) {
    Emitter emit(onProgress);
    TransferResult r;
    r.startedAt = nowIso();
    const uint64_t total = file.bytes.size();
    try {
        r.remotePath = body();
        r.ok = true;
        r.bytes = total;
        r.finishedAt = nowIso();
        emit(TransferState::Complete, r.remotePath.empty() ? "Done" : "Saved as " + r.remotePath, 1, total, total);
    } catch (const NetError& e) {
        r.ok = false;
        r.error = e.err;
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = makeError(TransferErrorCode::Unknown, std::string("Unexpected error: ") + e.what());
    } catch (...) {
        r.ok = false;
        r.error = makeError(TransferErrorCode::Unknown, "An unknown error occurred.");
    }
    if (!r.ok) {
        r.finishedAt = nowIso();
        emit(TransferState::Failed, r.error->message);
    }
    return r;
}

void ensureCurl() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::vector<std::string> lanLimitations() {
    return {"The device must show the File Transfer screen and be on the same network as this computer."};
}

std::string HttpResponse::header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? std::string() : it->second;
}

std::string baseUrl(const DeviceProfile& p) {
    return "http://" + p.host + ":" + std::to_string(p.port);
}

std::string encodeComponent(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '!' || c == '*' ||
            c == '\'' || c == '(' || c == ')') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string encodePath(const std::string& path) {
    bool trailing = !path.empty() && path.back() == '/';
    std::vector<std::string> segs;
    std::string cur;
    for (char c : path) {
        if (c == '/') {
            if (!cur.empty()) segs.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) segs.push_back(cur);
    std::string out;
    for (auto& s : segs) out += "/" + encodeComponent(s);
    if (out.empty()) return "/";
    if (trailing) out += "/";
    return out;
}

std::string formEncode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out;
    for (auto& [k, v] : kv) {
        if (!out.empty()) out += '&';
        out += encodeComponent(k) + "=" + encodeComponent(v);
    }
    return out;
}

TransferErrorCode statusCode(long status) {
    if (status == 401 || status == 403) return TransferErrorCode::Auth;
    if (status == 404) return TransferErrorCode::NotFound;
    return TransferErrorCode::Device;
}

NetError httpError(long status, const std::string& what, const std::string& body) {
    auto code = statusCode(status);
    std::string b = ks::trim(body);
    if (b.size() > 200) b.resize(200);
    std::string detail = b.empty() ? "" : ": " + b;
    std::string s = std::to_string(status);
    std::string text;
    if (code == TransferErrorCode::Auth)
        text = what + " was refused (HTTP " + s + "). Check the username and the password.";
    else if (code == TransferErrorCode::NotFound)
        text = what + " failed: not found (HTTP 404)" + detail + ".";
    else
        text = what + " failed: the device returned HTTP " + s + detail + ".";
    return NetError(code, text);
}

NetError networkError(const std::string& host) {
    return NetError(TransferErrorCode::Network,
                    "Could not reach " + host +
                        ". Make sure the device shows the File Transfer screen and is on the same Wi-Fi.");
}

namespace {

struct CurlCtx {
    const HttpRequest* req = nullptr;
    HttpResponse* resp = nullptr;
    const std::string* upload = nullptr;
    size_t readPos = 0;
};

size_t writeCb(char* ptr, size_t size, size_t n, void* ud) {
    auto* c = static_cast<CurlCtx*>(ud);
    c->resp->body.append(ptr, size * n);
    return size * n;
}

size_t headerCb(char* ptr, size_t size, size_t n, void* ud) {
    auto* c = static_cast<CurlCtx*>(ud);
    std::string line(ptr, size * n);
    // A new status line starts a new header block (redirects, 100 Continue).
    if (line.rfind("HTTP/", 0) == 0) c->resp->headers.clear();
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = ks::toLower(ks::trim(line.substr(0, colon)));
        c->resp->headers[name] = ks::trim(line.substr(colon + 1));
    }
    return size * n;
}

size_t readCb(char* buf, size_t size, size_t n, void* ud) {
    auto* c = static_cast<CurlCtx*>(ud);
    const std::string& body = *c->upload;
    size_t left = body.size() - c->readPos;
    size_t take = std::min(left, size * n);
    if (take) std::memcpy(buf, body.data() + c->readPos, take);
    c->readPos += take;
    return take;
}

int xferCb(void* ud, curl_off_t, curl_off_t, curl_off_t ultotal, curl_off_t ulnow) {
    auto* c = static_cast<CurlCtx*>(ud);
    if (isCancelled(c->req->cancel)) return 1;   // aborts with CURLE_ABORTED_BY_CALLBACK
    if (c->req->onUpload && ultotal > 0)
        c->req->onUpload(static_cast<uint64_t>(ulnow), static_cast<uint64_t>(ultotal));
    return 0;
}

} // namespace

HttpResponse httpRequest(const HttpRequest& r, const std::string& host) {
    ensureCurl();
    if (isCancelled(r.cancel)) throw cancelled();
    CURL* h = curl_easy_init();
    if (!h) throw NetError(TransferErrorCode::Unknown, "Could not start libcurl.");
    HttpResponse resp;
    static const std::string kEmpty;
    CurlCtx ctx;
    ctx.req = &r;
    ctx.resp = &resp;
    ctx.upload = r.body ? r.body : &kEmpty;

    curl_easy_setopt(h, CURLOPT_URL, r.url.c_str());
    curl_easy_setopt(h, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
    if (r.timeoutSec > 0) {
        curl_easy_setopt(h, CURLOPT_TIMEOUT, r.timeoutSec);
    } else {
        // No total timeout for uploads. Stop when the link stalls for 60 s.
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferCb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROXY, "*");   // the device is on the LAN: do not use a proxy
    if (!r.username.empty()) {
        curl_easy_setopt(h, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC));
        curl_easy_setopt(h, CURLOPT_USERNAME, r.username.c_str());
        curl_easy_setopt(h, CURLOPT_PASSWORD, r.password.c_str());
    }

    curl_slist* hdrs = nullptr;
    // The ESP32 server does not answer "Expect: 100-continue". Do not send it.
    hdrs = curl_slist_append(hdrs, "Expect:");
    for (auto& s : r.headers) hdrs = curl_slist_append(hdrs, s.c_str());

    curl_mime* mime = nullptr;
    if (r.method == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (r.upload) {
        curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(h, CURLOPT_READFUNCTION, readCb);
        curl_easy_setopt(h, CURLOPT_READDATA, &ctx);
        curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(ctx.upload->size()));
        if (r.method != "PUT") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, r.method.c_str());
    } else if (r.fileBytes) {
        mime = curl_mime_init(h);
        curl_mimepart* part = curl_mime_addpart(mime);
        curl_mime_name(part, "file");
        curl_mime_data(part, r.fileBytes->data(), r.fileBytes->size());
        curl_mime_filename(part, r.fileName.c_str());
        curl_mime_type(part, r.fileMime.empty() ? "application/octet-stream" : r.fileMime.c_str());
        curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
    } else if (r.body) {
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, r.body->data());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(r.body->size()));
        if (r.method != "POST") curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, r.method.c_str());
    } else if (r.method != "GET") {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, r.method.c_str());
    }
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    char errbuf[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, errbuf);
    CURLcode rc = curl_easy_perform(h);
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &resp.status);
    curl_easy_cleanup(h);
    if (mime) curl_mime_free(mime);
    curl_slist_free_all(hdrs);

    if (rc == CURLE_OK) return resp;
    std::string detail = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    switch (rc) {
        case CURLE_ABORTED_BY_CALLBACK:
            throw cancelled();
        case CURLE_OPERATION_TIMEDOUT:
            throw NetError(TransferErrorCode::NotFound,
                           host + " did not answer in time. Check the address and that the device is awake (" +
                               detail + ").");
        case CURLE_COULDNT_RESOLVE_HOST:
            throw NetError(TransferErrorCode::NotFound,
                           "Could not find " + host +
                               ". If crosspoint.local does not work, use the IP address shown on the reader.");
        case CURLE_UNSUPPORTED_PROTOCOL:
            throw NetError(TransferErrorCode::Unsupported, "libcurl does not support this protocol: " + detail);
        default: {
            NetError e = networkError(host);
            e.err.message += " (" + detail + ")";
            throw e;
        }
    }
}

} // namespace detail
} // namespace ks::net
