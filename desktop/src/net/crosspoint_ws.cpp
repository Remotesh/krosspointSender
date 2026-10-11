// CrossPoint WebSocket upload on port 81 (libcurl WebSocket API).
// Protocol:
//   client -> text   START:<filename>:<size>:<path>
//   device -> text   READY
//   client -> binary frames (16 KiB chunks)
//   device -> text   PROGRESS:<received>:<total>   (many)
//   device -> text   DONE | ERROR:<message>
// curl_ws_send blocks the flow when the socket buffer is full (CURLE_AGAIN).
// Then we wait until the socket is writable. This is the flow control.
#include "net/net_internal.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace ks::net {

namespace {
std::atomic<int> g_wsPort{81};
}

int wsPort() { return g_wsPort.load(); }
void setWsPort(int port) { g_wsPort.store(port > 0 && port < 65536 ? port : 81); }

namespace detail {

namespace {

constexpr size_t kChunk = 16 * 1024;
constexpr int kReadyTimeoutMs = 5000;
constexpr int kDoneTimeoutMs = 30000;

bool curlHasWs() {
    ensureCurl();
    const curl_version_info_data* v = curl_version_info(CURLVERSION_NOW);
    if (!v || !v->protocols) return false;
    for (const char* const* p = v->protocols; *p; ++p)
        if (std::strcmp(*p, "ws") == 0) return true;
    return false;
}

NetError unsupported() {
    return NetError(TransferErrorCode::Unsupported,
                    "This build of libcurl has no WebSocket support. Use WebDAV or CrossPoint HTTP instead.");
}

// One WebSocket connection. Closes in the destructor.
class WsConn {
public:
    WsConn(const std::string& host, const bool* cancel) : host_(host), cancel_(cancel) {}
    ~WsConn() {
        if (h_) {
            if (open_) {
                size_t sent = 0;
                curl_ws_send(h_, "", 0, &sent, 0, CURLWS_CLOSE);
            }
            curl_easy_cleanup(h_);
        }
    }
    WsConn(const WsConn&) = delete;
    WsConn& operator=(const WsConn&) = delete;

    void connect(int port) {
        ensureCurl();
        h_ = curl_easy_init();
        if (!h_) throw NetError(TransferErrorCode::Unknown, "Could not start libcurl.");
        std::string url = "ws://" + host_ + ":" + std::to_string(port) + "/";
        curl_easy_setopt(h_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(h_, CURLOPT_CONNECT_ONLY, 2L);   // 2 = WebSocket handshake, then manual frames
        curl_easy_setopt(h_, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
        curl_easy_setopt(h_, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(h_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h_, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(h_, CURLOPT_USERAGENT, kUserAgent);
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(h_, CURLOPT_ERRORBUFFER, errbuf);
        CURLcode rc = curl_easy_perform(h_);
        curl_easy_setopt(h_, CURLOPT_ERRORBUFFER, nullptr);
        if (rc == CURLE_UNSUPPORTED_PROTOCOL) throw unsupported();
        if (rc == CURLE_OPERATION_TIMEDOUT)
            throw NetError(TransferErrorCode::NotFound,
                           host_ + ":" + std::to_string(port) +
                               " did not answer in time. Check the address and that the device shows the File Transfer screen.");
        if (rc == CURLE_COULDNT_RESOLVE_HOST)
            throw NetError(TransferErrorCode::NotFound,
                           "Could not find " + host_ + ". If crosspoint.local does not work, use the IP address shown on the reader.");
        if (rc != CURLE_OK) {
            NetError e = networkError(host_);
            e.err.message += " (WebSocket port " + std::to_string(port) + ": " +
                             (errbuf[0] ? std::string(errbuf) : std::string(curl_easy_strerror(rc))) + ")";
            throw e;
        }
        curl_easy_getinfo(h_, CURLINFO_ACTIVESOCKET, &sock_);
        open_ = true;
    }

    // Sends one complete frame.
    void sendFrame(const char* data, size_t len, unsigned int flags, int timeoutMs = 30000) {
        size_t off = 0;
        auto t0 = std::chrono::steady_clock::now();
        do {
            if (isCancelled(cancel_)) throw cancelled();
            size_t n = 0;
            CURLcode rc = curl_ws_send(h_, data + off, len - off, &n, 0, flags);
            if (rc == CURLE_AGAIN) {
                // Call again with the same buffer when the socket is writable.
                if (msSince(t0) > timeoutMs)
                    throw NetError(TransferErrorCode::Device, "The device stopped taking data over the WebSocket.");
                waitSocket(false, true, 50);
                continue;
            }
            if (rc != CURLE_OK) {
                open_ = false;
                throw NetError(TransferErrorCode::Device,
                               std::string("The WebSocket send failed: ") + curl_easy_strerror(rc) + ".");
            }
            off += n;
            if (n > 0) t0 = std::chrono::steady_clock::now();
        } while (off < len);
    }

    void sendText(const std::string& s) { sendFrame(s.data(), s.size(), CURLWS_TEXT); }

    // Reads one complete text message. Returns false when no full message
    // came within waitMs. Throws when the device closed the connection.
    bool recvText(std::string& out, int waitMs) {
        auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (isCancelled(cancel_)) throw cancelled();
            char buf[4096];
            size_t n = 0;
            const curl_ws_frame* meta = nullptr;
            CURLcode rc = curl_ws_recv(h_, buf, sizeof buf, &n, &meta);
            if (rc == CURLE_AGAIN) {
                int left = waitMs - msSince(t0);
                if (left <= 0) return false;
                waitSocket(true, false, left < 50 ? left : 50);
                continue;
            }
            if (rc != CURLE_OK) {
                open_ = false;
                throw NetError(TransferErrorCode::Device, "The device closed the connection before it confirmed the upload.");
            }
            if (!meta) continue;
            if (meta->flags & CURLWS_CLOSE) {
                open_ = false;
                throw NetError(TransferErrorCode::Device, "The device closed the connection before it confirmed the upload.");
            }
            if (meta->flags & CURLWS_TEXT || (meta->flags & CURLWS_CONT && textOpen_)) {
                partial_.append(buf, n);
                textOpen_ = true;
                if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                    out = trim(partial_);
                    partial_.clear();
                    textOpen_ = false;
                    return true;
                }
            }
            // Binary, ping and pong frames: ignore. libcurl answers pings.
        }
    }

private:
    void waitSocket(bool forRead, bool forWrite, int ms) {
        if (sock_ == CURL_SOCKET_BAD) {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return;
        }
        fd_set rs, ws;
        FD_ZERO(&rs);
        FD_ZERO(&ws);
        if (forRead) FD_SET(sock_, &rs);
        if (forWrite) FD_SET(sock_, &ws);
        timeval tv;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        select(static_cast<int>(sock_ + 1), forRead ? &rs : nullptr, forWrite ? &ws : nullptr, nullptr, &tv);
    }

    std::string host_;
    const bool* cancel_;
    CURL* h_ = nullptr;
    curl_socket_t sock_ = CURL_SOCKET_BAD;
    bool open_ = false;
    std::string partial_;
    bool textOpen_ = false;
};

// Handles one device message during the upload. Returns true on DONE.
bool handleMessage(const std::string& msg, const Emitter& emit, uint64_t total, uint64_t& deviceRecv) {
    if (msg == "DONE") return true;
    if (msg.rfind("ERROR", 0) == 0) {
        auto colon = msg.find(':');
        std::string text = colon == std::string::npos ? "" : trim(msg.substr(colon + 1));
        if (text.empty()) text = "unknown error";
        auto code = toLower(text).find("exist") != std::string::npos ? TransferErrorCode::Exists : TransferErrorCode::Device;
        throw NetError(code, "The device reported an error: " + text);
    }
    if (msg.rfind("PROGRESS:", 0) == 0) {
        auto a = msg.find(':');
        auto b = msg.find(':', a + 1);
        try {
            uint64_t r = std::stoull(msg.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1));
            uint64_t t = b == std::string::npos ? total : std::stoull(msg.substr(b + 1));
            if (t == 0) t = total;
            deviceRecv = r;
            if (r >= t) emit(TransferState::Verifying, "Waiting for the device to confirm", 1, r, t);
        } catch (...) {
            // Ignore a bad PROGRESS line.
        }
    }
    return false;
}

} // namespace

class CrossPointWsTransport final : public Transport {
public:
    TransferMethod method() const override { return TransferMethod::CrossPointWs; }
    bool isSupported() const override { return curlHasWs(); }
    std::vector<std::string> limitations() const override {
        auto out = lanLimitations();
        out.push_back("The WebSocket upload uses port " + std::to_string(wsPort()) + ". The port in the profile is used only for the status check.");
        if (!isSupported()) out.insert(out.begin(), "This build of libcurl has no WebSocket support.");
        return out;
    }

    ConnectionTestResult testConnection(const DeviceProfile& p) override {
        auto t0 = std::chrono::steady_clock::now();
        ConnectionTestResult res = crosspointStatus(p);
        if (!res.ok) return res;
        try {
            if (!curlHasWs()) throw unsupported();
            WsConn ws(p.host, nullptr);
            ws.connect(wsPort());
            auto j = nlohmann::json::parse(res.info, nullptr, false);
            if (j.is_object()) {
                j["wsPort"] = wsPort();
                res.info = j.dump();
            }
        } catch (const NetError& e) {
            res.ok = false;
            res.error = e.err;
        }
        res.latencyMs = msSince(t0);
        return res;
    }

    TransferResult send(const DeviceProfile& p, const OutputFile& file, const ProgressFn& onProgress,
                        const bool* cancel) override {
        return runSend(file, onProgress, [&]() -> std::string {
            Emitter emit(onProgress);
            if (isCancelled(cancel)) throw cancelled();
            if (!curlHasWs()) throw unsupported();
            if (file.name.find_first_of(":\r\n") != std::string::npos)
                throw NetError(TransferErrorCode::Device,
                               "The file name contains \":\" or a line break. The WebSocket protocol cannot send it.");
            const uint64_t total = file.bytes.size();
            const std::string dir = folderNoTrailingSlash(p.targetFolder);
            const int port = wsPort();

            emit(TransferState::Connecting, "Connecting to " + p.host + ":" + std::to_string(port));
            WsConn ws(p.host, cancel);
            ws.connect(port);
            ws.sendText("START:" + file.name + ":" + std::to_string(total) + ":" + dir);

            std::string msg;
            for (;;) {
                if (!ws.recvText(msg, kReadyTimeoutMs))
                    throw NetError(TransferErrorCode::NotFound,
                                   p.host + ":" + std::to_string(port) + " did not answer READY within 5 s.");
                if (msg == "READY") break;
                uint64_t ignored = 0;
                handleMessage(msg, emit, total, ignored);   // throws on ERROR
            }

            emit(TransferState::Uploading, "Uploading " + file.name, 0, 0, total);
            uint64_t deviceRecv = 0;
            bool done = false;
            size_t off = 0;
            while (off < file.bytes.size() && !done) {
                size_t n = std::min(kChunk, file.bytes.size() - off);
                ws.sendFrame(file.bytes.data() + off, n, CURLWS_BINARY);
                off += n;
                emit(TransferState::Uploading, {}, total ? static_cast<double>(off) / static_cast<double>(total) : 1,
                     off, total);
                // Read the device messages that are already here. Do not wait.
                while (!done && ws.recvText(msg, 0)) done = handleMessage(msg, emit, total, deviceRecv);
            }
            if (!done) emit(TransferState::Verifying, "Waiting for the device to confirm", 1, total, total);
            auto t0 = std::chrono::steady_clock::now();
            while (!done) {
                int left = kDoneTimeoutMs - msSince(t0);
                if (left <= 0 || !ws.recvText(msg, left))
                    throw NetError(TransferErrorCode::Device, "The device did not confirm the upload within 30 s.");
                done = handleMessage(msg, emit, total, deviceRecv);
            }
            return remoteFilePath(p.targetFolder, file.name);
        });
    }
};

Transport& crosspointWsTransport() {
    static CrossPointWsTransport t;
    return t;
}

} // namespace detail
} // namespace ks::net
