// Tests for src/net: storage, explain, discovery parser, transports against
// an in-process fake CrossPoint device, and the helper server.
//
// The fake device is a small raw-socket HTTP server, not cpp-httplib:
// cpp-httplib 0.18 rejects unknown methods such as MKCOL with 400 (see
// Server::parse_request_line). The fake also serves the WebSocket upload.
#include "net/net.h"
#include "check.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace ks;
using nlohmann::json;
namespace fs = std::filesystem;

// ============================================================== socket helpers

#ifdef _WIN32
using sock_t = SOCKET;
static const sock_t kBad = INVALID_SOCKET;
static void closeSock(sock_t s) { closesocket(s); }
#else
using sock_t = int;
static const sock_t kBad = -1;
static void closeSock(sock_t s) { close(s); }
#endif

static bool recvAll(sock_t s, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = static_cast<int>(recv(s, buf + got, static_cast<int>(n - got), 0));
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

static void sendAll(sock_t s, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int r = static_cast<int>(send(s, data.data() + off, static_cast<int>(data.size() - off), 0));
        if (r <= 0) return;
        off += static_cast<size_t>(r);
    }
}

// ============================================================== SHA-1 + base64 (WebSocket handshake)

static std::string sha1(const std::string& msg) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = msg;
    uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; --i) m += static_cast<char>((bitLen >> (i * 8)) & 0xFF);
    auto rol = [](uint32_t v, int c) { return (v << c) | (v >> (32 - c)); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(m[off + i * 4])) << 24) | (uint32_t(uint8_t(m[off + i * 4 + 1])) << 16) |
                   (uint32_t(uint8_t(m[off + i * 4 + 2])) << 8) | uint32_t(uint8_t(m[off + i * 4 + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::string out;
    for (uint32_t v : h)
        for (int i = 3; i >= 0; --i) out += static_cast<char>((v >> (i * 8)) & 0xFF);
    return out;
}

static std::string base64(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8) | uint8_t(in[i + 2]);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint32_t(uint8_t(in[i])) << 16) | (uint32_t(uint8_t(in[i + 1])) << 8);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// ============================================================== URL helpers

static std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::map<std::string, std::string> parseForm(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t amp = s.find('&', start);
        std::string kv = s.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        auto eq = kv.find('=');
        if (eq != std::string::npos) out[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return out;
}

static std::string joinDir(const std::string& dir, const std::string& name) {
    return dir == "/" ? "/" + name : dir + "/" + name;
}

// ============================================================== fake CrossPoint device

class FakeDevice {
public:
    std::mutex m;
    std::map<std::string, std::string> files;   // "/Books/Inbox/x.epub" -> bytes
    std::set<std::string> dirs{"/"};             // without trailing slash, "/" for root
    int deletes = 0, uploads = 0, puts = 0, mkcols = 0, wsUploads = 0, propfinds = 0;
    bool allowDelete = true;

    bool start() {
        lsock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (lsock_ == kBad) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(lsock_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return false;
        if (listen(lsock_, 16) != 0) return false;
#ifdef _WIN32
        int len = sizeof a;
#else
        socklen_t len = sizeof a;
#endif
        getsockname(lsock_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        stop_ = true;
        if (th_.joinable()) th_.join();
        closeSock(lsock_);
    }

    int port() const { return port_; }

    std::string file(const std::string& path) {
        std::lock_guard<std::mutex> lk(m);
        auto it = files.find(path);
        return it == files.end() ? std::string("<missing>") : it->second;
    }

private:
    sock_t lsock_ = kBad;
    int port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};

    void loop() {
        while (!stop_) {
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(lsock_, &rs);
            timeval tv{0, 50000};
            if (select(static_cast<int>(lsock_ + 1), &rs, nullptr, nullptr, &tv) <= 0) continue;
            sock_t c = accept(lsock_, nullptr, nullptr);
            if (c == kBad) continue;
            // One connection at a time is enough for the tests.
            handle(c);
            closeSock(c);
        }
    }

    static void respond(sock_t c, int status, const std::string& reason, const std::string& body,
                        const std::string& extra = "", const std::string& type = "text/plain", bool head = false,
                        size_t headLen = 0) {
        std::string r = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
        r += "Content-Type: " + type + "\r\n";
        r += "Content-Length: " + std::to_string(head ? headLen : body.size()) + "\r\n";
        r += "Access-Control-Allow-Origin: *\r\nConnection: close\r\n" + extra + "\r\n";
        if (!head) r += body;
        sendAll(c, r);
    }

    void handle(sock_t c) {
#ifdef _WIN32
        DWORD to = 5000;
#else
        timeval to{5, 0};
#endif
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        std::string data;
        char buf[8192];
        size_t headEnd;
        while ((headEnd = data.find("\r\n\r\n")) == std::string::npos) {
            int r = static_cast<int>(recv(c, buf, sizeof buf, 0));
            if (r <= 0) return;
            data.append(buf, static_cast<size_t>(r));
        }
        std::string head = data.substr(0, headEnd);
        std::string body = data.substr(headEnd + 4);
        std::map<std::string, std::string> hdr;
        size_t lineEnd = head.find("\r\n");
        std::string reqLine = head.substr(0, lineEnd);
        size_t pos = lineEnd == std::string::npos ? head.size() : lineEnd + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            std::string line = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            auto colon = line.find(':');
            if (colon != std::string::npos) hdr[toLower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
            if (e == std::string::npos) break;
            pos = e + 2;
        }
        size_t contentLen = hdr.count("content-length") ? std::stoul(hdr["content-length"]) : 0;
        while (body.size() < contentLen) {
            int r = static_cast<int>(recv(c, buf, sizeof buf, 0));
            if (r <= 0) return;
            body.append(buf, static_cast<size_t>(r));
        }
        auto sp1 = reqLine.find(' ');
        auto sp2 = reqLine.find(' ', sp1 + 1);
        std::string method = reqLine.substr(0, sp1);
        std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string query;
        auto q = target.find('?');
        if (q != std::string::npos) {
            query = target.substr(q + 1);
            target = target.substr(0, q);
        }
        std::string path = urlDecode(target);
        auto params = parseForm(query);

        if (toLower(hdr["upgrade"]) == "websocket") return websocket(c, hdr["sec-websocket-key"]);

        std::lock_guard<std::mutex> lk(m);
        if (method == "OPTIONS") return respond(c, 204, "No Content", "", "DAV: 1\r\nAllow: OPTIONS, GET, HEAD, PUT, MKCOL\r\n");
        if (method == "PROPFIND") {
            ++propfinds;
            std::string d = path.size() > 1 && path.back() == '/' ? path.substr(0, path.size() - 1) : path;
            if (!dirs.count(d)) return respond(c, 404, "Not Found", "no folder");
            std::string prefix = d == "/" ? "/" : d + "/";
            // Use the "lp1" prefix, not "D", so that the parser must accept any prefix.
            std::string x = "<?xml version=\"1.0\"?><lp1:multistatus xmlns:lp1=\"DAV:\">";
            auto entry = [&](const std::string& href, bool dir, size_t size) {
                x += "<lp1:response><lp1:href>" + href + "</lp1:href><lp1:propstat><lp1:prop><lp1:resourcetype>";
                if (dir) x += "<lp1:collection/>";
                x += "</lp1:resourcetype>";
                if (!dir) x += "<lp1:getcontentlength>" + std::to_string(size) + "</lp1:getcontentlength>";
                x += "</lp1:prop><lp1:status>HTTP/1.1 200 OK</lp1:status></lp1:propstat></lp1:response>";
            };
            entry(prefix, true, 0);
            for (auto& sd : dirs) {
                if (sd == "/" || sd.compare(0, prefix.size(), prefix) != 0) continue;
                std::string rest = sd.substr(prefix.size());
                if (rest.empty() || rest.find('/') != std::string::npos) continue;
                entry("http://127.0.0.1" + prefix + rest + "/", true, 0);
            }
            for (auto& [fp, b] : files) {
                if (fp.compare(0, prefix.size(), prefix) != 0) continue;
                std::string rest = fp.substr(prefix.size());
                if (rest.find('/') != std::string::npos) continue;
                entry(prefix + rest, false, b.size());
            }
            x += "</lp1:multistatus>";
            return respond(c, 207, "Multi-Status", x, "", "application/xml");
        }
        if (method == "MKCOL") {
            ++mkcols;
            std::string d = path.size() > 1 && path.back() == '/' ? path.substr(0, path.size() - 1) : path;
            if (dirs.count(d)) return respond(c, 405, "Method Not Allowed", "exists");
            dirs.insert(d);
            return respond(c, 201, "Created", "");
        }
        if (method == "PUT") {
            ++puts;
            std::string parent = path.substr(0, path.find_last_of('/'));
            if (parent.empty()) parent = "/";
            if (!dirs.count(parent)) return respond(c, 409, "Conflict", "no parent");
            bool existed = files.count(path) > 0;
            files[path] = body;
            return respond(c, existed ? 204 : 201, existed ? "No Content" : "Created", "");
        }
        if (method == "HEAD") {
            auto it = files.find(path);
            if (it == files.end()) return respond(c, 404, "Not Found", "", "", "text/plain", true, 0);
            return respond(c, 200, "OK", "", "", "application/epub+zip", true, it->second.size());
        }
        if (method == "GET" && path == "/api/status")
            return respond(c, 200, "OK",
                           R"({"version":"1.2.0-fake","ip":"127.0.0.1","mode":"STA","rssi":-50,"freeHeap":100000,"uptime":5,"device":"x4"})",
                           "", "application/json");
        if (method == "GET" && path == "/api/files") {
            std::string dir = params.count("path") ? params["path"] : "/";
            json arr = json::array();
            std::string prefix = dir == "/" ? "/" : dir + "/";
            for (auto& sd : dirs) {
                if (sd == "/" || sd.compare(0, prefix.size(), prefix) != 0) continue;
                std::string rest = sd.substr(prefix.size());
                if (rest.empty() || rest.find('/') != std::string::npos) continue;
                arr.push_back({{"name", rest}, {"size", 0}, {"isDirectory", true}, {"isEpub", false}});
            }
            for (auto& [p, b] : files) {
                if (p.compare(0, prefix.size(), prefix) != 0) continue;
                std::string rest = p.substr(prefix.size());
                if (rest.find('/') != std::string::npos) continue;
                arr.push_back({{"name", rest}, {"size", b.size()}, {"isDirectory", false}, {"isEpub", true}});
            }
            return respond(c, 200, "OK", arr.dump(), "", "application/json");
        }
        if (method == "POST" && path == "/mkdir") {
            auto f = parseForm(body);
            dirs.insert(joinDir(f["path"], f["name"]));
            return respond(c, 200, "OK", "Folder created");
        }
        if (method == "POST" && path == "/delete") {
            if (!allowDelete) return respond(c, 500, "Error", "no");
            auto f = parseForm(body);
            json paths = json::parse(f["paths"], nullptr, false);
            if (paths.is_array())
                for (auto& p : paths) files.erase(p.get<std::string>());
            ++deletes;
            return respond(c, 200, "OK", "Deleted");
        }
        if (method == "POST" && path == "/upload") {
            ++uploads;
            std::string dir = params.count("path") ? params["path"] : "/";
            std::string ctype = hdr["content-type"];
            auto bpos = ctype.find("boundary=");
            if (bpos == std::string::npos) return respond(c, 400, "Bad Request", "no boundary");
            std::string boundary = "--" + ctype.substr(bpos + 9);
            auto partStart = body.find(boundary);
            auto partHeadEnd = body.find("\r\n\r\n", partStart);
            std::string partHead = body.substr(partStart, partHeadEnd - partStart);
            auto fnPos = partHead.find("filename=\"");
            std::string name = partHead.substr(fnPos + 10, partHead.find('"', fnPos + 10) - fnPos - 10);
            auto dataStart = partHeadEnd + 4;
            auto dataEnd = body.find("\r\n" + boundary, dataStart);
            std::string full = joinDir(dir, name);
            if (!dirs.count(dir)) return respond(c, 400, "Bad Request", "Folder does not exist");
            if (files.count(full)) return respond(c, 400, "Bad Request", "File already exists: " + name);
            files[full] = body.substr(dataStart, dataEnd - dataStart);
            return respond(c, 200, "OK", "File uploaded successfully: " + name);
        }
        respond(c, 404, "Not Found", "not found");
    }

    static void wsSend(sock_t c, const std::string& text) {
        std::string f;
        f += static_cast<char>(0x81);
        if (text.size() < 126) {
            f += static_cast<char>(text.size());
        } else {
            f += static_cast<char>(126);
            f += static_cast<char>((text.size() >> 8) & 0xFF);
            f += static_cast<char>(text.size() & 0xFF);
        }
        f += text;
        sendAll(c, f);
    }

    void websocket(sock_t c, const std::string& key) {
        std::string accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
        sendAll(c, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                   "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
        std::string name, dir, data;
        uint64_t size = 0;
        bool started = false;
        for (;;) {
            unsigned char h2[2];
            if (!recvAll(c, reinterpret_cast<char*>(h2), 2)) return;
            int opcode = h2[0] & 0x0F;
            uint64_t len = h2[1] & 0x7F;
            if (len == 126) {
                unsigned char e[2];
                if (!recvAll(c, reinterpret_cast<char*>(e), 2)) return;
                len = (uint64_t(e[0]) << 8) | e[1];
            } else if (len == 127) {
                unsigned char e[8];
                if (!recvAll(c, reinterpret_cast<char*>(e), 8)) return;
                len = 0;
                for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
            }
            unsigned char mask[4] = {0, 0, 0, 0};
            if (h2[1] & 0x80 && !recvAll(c, reinterpret_cast<char*>(mask), 4)) return;
            std::string payload(static_cast<size_t>(len), '\0');
            if (len && !recvAll(c, &payload[0], static_cast<size_t>(len))) return;
            for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            if (opcode == 0x8) {
                sendAll(c, std::string("\x88\x00", 2));
                return;
            }
            if (opcode == 0x1 && payload.rfind("START:", 0) == 0) {
                // START:<name>:<size>:<path>
                auto a = payload.find(':', 6);
                auto b = payload.find(':', a + 1);
                name = payload.substr(6, a - 6);
                size = std::stoull(payload.substr(a + 1, b - a - 1));
                dir = payload.substr(b + 1);
                started = true;
                wsSend(c, "READY");
                if (size == 0) goto finish;
                continue;
            }
            if ((opcode == 0x2 || opcode == 0x0) && started) {
                data += payload;
                wsSend(c, "PROGRESS:" + std::to_string(data.size()) + ":" + std::to_string(size));
                if (data.size() >= size) goto finish;
            }
            continue;
        finish : {
            std::lock_guard<std::mutex> lk(m);
            files[joinDir(dir, name)] = data;
            ++wsUploads;
        }
            wsSend(c, "DONE");
            started = false;
        }
    }
};

// ============================================================== helpers

static OutputFile makeFile(const std::string& name, size_t n) {
    OutputFile f;
    f.name = name;
    f.mime = "application/epub+zip";
    f.format = OutputFormat::Epub;
    f.bytes.resize(n);
    uint32_t x = 12345;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        f.bytes[i] = static_cast<char>((x >> 16) & 0xFF);
    }
    if (n > 10) f.bytes[5] = '\0';
    return f;
}

struct Recorder {
    std::vector<TransferProgress> events;
    std::mutex m;
    ProgressFn fn() {
        return [this](const TransferProgress& p) {
            std::lock_guard<std::mutex> lk(m);
            events.push_back(p);
        };
    }
    bool saw(TransferState s) {
        for (auto& e : events)
            if (e.state == s) return true;
        return false;
    }
    TransferState last() { return events.empty() ? TransferState::Idle : events.back().state; }
};

static DeviceProfile fakeProfile(const FakeDevice& d, TransferMethod m) {
    DeviceProfile p;
    p.name = "fake";
    p.host = "127.0.0.1";
    p.port = d.port();
    p.method = m;
    p.targetFolder = "/Books/Inbox/";
    return p;
}

// ============================================================== tests

static void testPure() {
    using namespace ks::net;
    CHECK(folderSegments("/Books//Inbox/") == (std::vector<std::string>{"Books", "Inbox"}));
    CHECK(folderSegments("\\a\\..\\b\\.") == (std::vector<std::string>{"a", "b"}));
    CHECK_EQ(normalizeFolder("Books/Inbox"), std::string("/Books/Inbox/"));
    CHECK_EQ(normalizeFolder(""), std::string("/"));
    CHECK_EQ(folderNoTrailingSlash("/Books/Inbox/"), std::string("/Books/Inbox"));
    CHECK_EQ(folderNoTrailingSlash("/"), std::string("/"));
    CHECK_EQ(remoteFilePath("/Books/Inbox", "a.epub"), std::string("/Books/Inbox/a.epub"));
    CHECK_EQ(crc32("123456789"), 0xCBF43926u);
    CHECK_EQ(toString(TransferErrorCode::NotFound), std::string("not-found"));
    CHECK(errorCodeFrom("exists") == TransferErrorCode::Exists);
    CHECK(transport(TransferMethod::WebDav).method() == TransferMethod::WebDav);
    CHECK(&transport(TransferMethod::Ble) == &transport(TransferMethod::Ble));
    CHECK_EQ(methods().size(), size_t(5));
    for (auto m : methods()) CHECK(transport(m).method() == m);

    // Discovery reply parser.
    Discovered d;
    CHECK(parseDiscoveryReply("crosspoint (on CrossPoint-X4);81", d));
    CHECK_EQ(d.hostname, std::string("CrossPoint-X4"));
    CHECK_EQ(d.wsPort, 81);
    CHECK(parseDiscoveryReply("crosspoint (on my reader);8181\n", d));
    CHECK_EQ(d.hostname, std::string("my reader"));
    CHECK_EQ(d.wsPort, 8181);
    CHECK(!parseDiscoveryReply("hello", d));
    CHECK(!parseDiscoveryReply("crosspoint (on x);abc", d));
    CHECK(!parseDiscoveryReply("crosspoint (on x;81", d));

    // Origin allow-list.
    std::vector<std::string> allowed = AppSettings{}.helperAllowedOrigins;
    CHECK(originAllowed("https://k-engine.org", allowed));
    CHECK(!originAllowed("https://k-engine.org.evil.com", allowed));
    CHECK(!originAllowed("https://k-engine.org:8443", allowed));
    CHECK(originAllowed("http://localhost", allowed));
    CHECK(originAllowed("http://localhost:5173", allowed));
    CHECK(originAllowed("http://127.0.0.1:8080", allowed));
    CHECK(!originAllowed("http://localhost.evil.com", allowed));
    CHECK(!originAllowed("http://localhost:5173x", allowed));
    CHECK(!originAllowed("null", allowed));
    CHECK(!originAllowed("", allowed));
    CHECK(!originAllowed("null", {"null"}));

    // explain()
    for (auto c : {TransferErrorCode::Network, TransferErrorCode::NotFound, TransferErrorCode::Auth,
                   TransferErrorCode::Exists, TransferErrorCode::Unsupported, TransferErrorCode::Cancelled,
                   TransferErrorCode::Device, TransferErrorCode::Unknown}) {
        TransferError e;
        e.code = c;
        e.message = "msg";
        auto x = explain(e);
        CHECK(!x.title.empty());
        CHECK(!x.reasons.empty());
        CHECK(!x.fallbacks.empty());
    }
    TransferError ne;
    ne.code = TransferErrorCode::Network;
    auto x = explain(ne);
    bool hotspot = false, folder = false, screen = false;
    for (auto& f : x.fallbacks) {
        if (f.find("192.168.4.1") != std::string::npos) hotspot = true;
        if (f.find("USB Drive mode") != std::string::npos) folder = true;
        if (f.find("File Transfer screen") != std::string::npos) screen = true;
    }
    CHECK(hotspot && folder && screen);
}

static void testProfiles() {
    using namespace ks::net;
    CHECK(isValidHost("crosspoint.local"));
    CHECK(isValidHost("192.168.4.1"));
    CHECK(!isValidHost("300.1.1.1"));
    CHECK(!isValidHost("bad host"));
    CHECK(!isValidHost("-bad.local"));
    CHECK(!isValidHost(""));

    DeviceProfile d = defaultProfile(FirmwareType::CrossPoint);
    CHECK(validateProfile(d).empty());
    CHECK_EQ(defaultProfile(FirmwareType::KReader).method, TransferMethod::Ble);
    CHECK(validateProfile(defaultProfile(FirmwareType::KReader)).empty());

    DeviceProfile bad = d;
    bad.name = "  ";
    bad.host = "http://x.local";
    bad.port = 70000;
    bad.targetFolder = "Books";
    CHECK_EQ(validateProfile(bad).size(), size_t(4));
    bad = d;
    bad.host = "";
    CHECK_EQ(validateProfile(bad).size(), size_t(1));
    bad.method = TransferMethod::Folder;   // host = local folder, required
    CHECK_EQ(validateProfile(bad).size(), size_t(1));
    bad.host = "E:/";
    CHECK(validateProfile(bad).empty());

    for (auto& p : listProfiles()) deleteProfile(p.id);
    CHECK(listProfiles().empty());

    bool threw = false;
    try {
        DeviceProfile b = d;
        b.port = 0;
        saveProfile(b);
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("port") != std::string::npos;
    }
    CHECK(threw);
    CHECK(listProfiles().empty());

    DeviceProfile p = d;
    p.name = "  My X4  ";
    p.host = " 192.168.1.50 ";
    p = saveProfile(p);
    CHECK(!p.id.empty());
    CHECK_EQ(p.name, std::string("My X4"));
    CHECK_EQ(p.host, std::string("192.168.1.50"));
    CHECK(!p.createdAt.empty());
    CHECK(fs::exists(fs::u8path(joinPath(dataDir(), "devices.json"))));

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // createdAt has second resolution
    DeviceProfile q = defaultProfile(FirmwareType::Other);
    q.host = "reader.lan";
    q.method = TransferMethod::CrossPointHttp;
    q = saveProfile(q);
    auto all = listProfiles();
    CHECK_EQ(all.size(), size_t(2));
    if (all.size() == 2) {
        CHECK_EQ(all[0].id, p.id);   // oldest first
        CHECK_EQ(all[1].method, TransferMethod::CrossPointHttp);
    }

    std::string created = p.createdAt;
    p.notes = "desk";
    p.createdAt = "";
    p = saveProfile(p);
    CHECK_EQ(p.createdAt, created);   // update keeps createdAt
    auto got = getProfile(p.id);
    CHECK(got.has_value());
    if (got) {
        CHECK_EQ(got->notes, std::string("desk"));
        CHECK_EQ(got->port, 80);
        CHECK_EQ(got->targetFolder, std::string("/Books/K-rosspoint/"));
    }
    CHECK(got && got->lastConnectedAt.empty());
    markConnected(p.id);
    got = getProfile(p.id);
    CHECK(got && !got->lastConnectedAt.empty());

    deleteProfile(p.id);
    CHECK(!getProfile(p.id).has_value());
    CHECK_EQ(listProfiles().size(), size_t(1));
    deleteProfile(q.id);
    CHECK(listProfiles().empty());
}

static void testSettings() {
    using namespace ks::net;
    std::error_code ec;
    fs::remove(fs::u8path(joinPath(dataDir(), "settings.json")), ec);
    AppSettings d = loadSettings();
    CHECK_EQ(d.helperPort, 8134);
    CHECK_EQ(d.helperAllowedOrigins.size(), size_t(3));

    AppSettings s;
    s.style.preset = StylePreset::Large;
    s.style.fontSizePercent = 120;
    s.style.lineHeight = 1.6;
    s.style.tableHandling = TableHandling::Linearize;
    s.style.customCss = "p { text-indent: 1em; }";
    s.filename.templ = "{date}-{title}";
    s.filename.lowercase = true;
    s.filename.spaceReplacement = "_";
    s.includeCover = false;
    s.extract.keepInfobox = true;
    s.defaultDeviceId = "dev-1";
    s.helperServerEnabled = false;
    s.helperPort = 9000;
    s.helperAllowedOrigins = {"https://example.org"};
    s.lastExportDir = "C:/Users/x/Ü";
    saveSettings(s);
    AppSettings r = loadSettings();
    CHECK(r.style.preset == StylePreset::Large);
    CHECK_EQ(r.style.fontSizePercent, 120);
    CHECK(r.style.lineHeight == 1.6);
    CHECK(r.style.tableHandling == TableHandling::Linearize);
    CHECK_EQ(r.style.customCss, s.style.customCss);
    CHECK_EQ(r.filename.templ, std::string("{date}-{title}"));
    CHECK(r.filename.lowercase);
    CHECK_EQ(r.filename.spaceReplacement, std::string("_"));
    CHECK(!r.includeCover);
    CHECK(r.extract.keepInfobox);
    CHECK_EQ(r.defaultDeviceId, std::string("dev-1"));
    CHECK(!r.helperServerEnabled);
    CHECK_EQ(r.helperPort, 9000);
    CHECK(r.helperAllowedOrigins == std::vector<std::string>{"https://example.org"});
    CHECK_EQ(r.lastExportDir, s.lastExportDir);

    // A partial file merges over the defaults.
    writeFile(joinPath(dataDir(), "settings.json"), R"({"helperPort": 8200, "style": {"fontSizePercent": 90}})");
    r = loadSettings();
    CHECK_EQ(r.helperPort, 8200);
    CHECK_EQ(r.style.fontSizePercent, 90);
    CHECK(r.style.lineHeight == 1.45);
    CHECK(r.helperServerEnabled);
    // Bad JSON gives the defaults.
    writeFile(joinPath(dataDir(), "settings.json"), "{not json");
    CHECK_EQ(loadSettings().helperPort, 8134);
}

static void testLibrary() {
    using namespace ks::net;
    clearLibrary();
    CHECK(listEntries().empty());

    LibraryEntry a;
    a.article.id = newUuid();
    a.article.title = "First";
    a.article.bodyHtml = "<p>Hello &amp; bye</p>";
    a.article.tags = {"x", "y"};
    a.article.toc = {{"sec-1", 2, "Intro"}};
    a.article.images = {{"https://e.com/a.jpg", "A"}};
    a.article.inputKind = InputKind::Wikipedia;
    a.fileName = "first.epub";
    a.updatedAt = "2026-10-01T10:00:00Z";
    ImageMap imgs = {{"https://e.com/a.jpg", std::string("\xff\xd8\xff\x00jpeg", 8)}, {"data:x", "png"}};
    saveEntry(a, &imgs);

    LibraryEntry b;
    b.article.id = newUuid();
    b.article.title = "Second";
    b.updatedAt = "2026-10-02T10:00:00Z";
    saveEntry(b);

    auto list = listEntries();
    CHECK_EQ(list.size(), size_t(2));
    if (list.size() == 2) {
        CHECK_EQ(list[0].article.title, std::string("Second"));   // newest first
        CHECK_EQ(list[1].article.title, std::string("First"));
    }
    auto g = getEntry(a.article.id);
    CHECK(g.has_value());
    if (g) {
        CHECK_EQ(g->article.bodyHtml, a.article.bodyHtml);
        CHECK(g->article.tags == a.article.tags);
        CHECK_EQ(g->article.toc.size(), size_t(1));
        CHECK(g->article.inputKind == InputKind::Wikipedia);
        CHECK_EQ(g->article.images.size(), size_t(1));
        CHECK(!g->savedAt.empty());
    }
    auto li = loadImages(a.article.id);
    CHECK(li == imgs);
    CHECK(fs::exists(fs::u8path(joinPath(dataDir(), "library/" + a.article.id + "/images/001.bin"))));
    // Save without images keeps the stored images.
    saveEntry(a);
    CHECK(loadImages(a.article.id) == imgs);
    // Save with an empty map removes them.
    ImageMap none;
    saveEntry(a, &none);
    CHECK(loadImages(a.article.id).empty());

    TransferRecord r;
    r.deviceName = "X4";
    r.method = TransferMethod::CrossPointHttp;
    r.fileName = "first.epub";
    r.bytes = 1234;
    r.ok = true;
    addTransfer(a.article.id, r);
    g = getEntry(a.article.id);
    CHECK(g && g->transfers.size() == 1);
    if (g && g->transfers.size() == 1) {
        CHECK_EQ(g->transfers[0].articleId, a.article.id);
        CHECK(g->transfers[0].method == TransferMethod::CrossPointHttp);
        CHECK_EQ(g->transfers[0].bytes, uint64_t(1234));
        CHECK(!g->transfers[0].id.empty());
        CHECK(g->updatedAt > a.updatedAt);
    }
    bool threw = false;
    try {
        addTransfer("no-such-id", r);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        LibraryEntry bad;
        bad.article.id = "../evil";
        saveEntry(bad);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);

    deleteEntry(b.article.id);
    CHECK(!getEntry(b.article.id).has_value());
    CHECK_EQ(listEntries().size(), size_t(1));
    clearLibrary();
    CHECK(listEntries().empty());
}

static void testWebDav(FakeDevice& dev) {
    using namespace ks::net;
    auto& t = transport(TransferMethod::WebDav);
    DeviceProfile p = fakeProfile(dev, TransferMethod::WebDav);
    auto tc = t.testConnection(p);
    CHECK(tc.ok);
    CHECK(tc.info.find("\"status\":204") != std::string::npos);

    p.targetFolder = "/Books/In box/";
    OutputFile f = makeFile("Test \xc3\xbc #1.epub", 300000);
    Recorder rec;
    auto r = t.send(p, f, rec.fn());
    CHECK(r.ok);
    if (!r.ok && r.error) std::fprintf(stderr, "webdav error: %s\n", r.error->message.c_str());
    CHECK_EQ(r.remotePath, std::string("/Books/In box/Test \xc3\xbc #1.epub"));
    CHECK_EQ(r.bytes, uint64_t(300000));
    CHECK(dev.file("/Books/In box/Test \xc3\xbc #1.epub") == f.bytes);
    CHECK(rec.saw(TransferState::Connecting));
    CHECK(rec.saw(TransferState::Uploading));
    CHECK(rec.saw(TransferState::Verifying));
    CHECK(rec.last() == TransferState::Complete);
    bool sawFraction = false;
    for (auto& e : rec.events)
        if (e.state == TransferState::Uploading && e.fraction > 0.99) sawFraction = true;
    CHECK(sawFraction);

    // Second send overwrites (folders already exist -> 405 is fine).
    f.bytes = "short";
    r = t.send(p, f, nullptr);
    CHECK(r.ok);
    CHECK(dev.file("/Books/In box/Test \xc3\xbc #1.epub") == "short");

    // Cancel before the start.
    bool cancel = true;
    Recorder rc;
    r = t.send(p, f, rc.fn(), &cancel);
    CHECK(!r.ok);
    CHECK(r.error && r.error->code == TransferErrorCode::Cancelled);
    CHECK(rc.last() == TransferState::Failed);
}

static void testCrossPointHttp(FakeDevice& dev) {
    using namespace ks::net;
    auto& t = transport(TransferMethod::CrossPointHttp);
    DeviceProfile p = fakeProfile(dev, TransferMethod::CrossPointHttp);
    p.targetFolder = "/Books/Http/";
    auto tc = t.testConnection(p);
    CHECK(tc.ok);
    CHECK(tc.info.find("1.2.0-fake") != std::string::npos);

    OutputFile f = makeFile("book one.epub", 120000);
    Recorder rec;
    auto r = t.send(p, f, rec.fn());
    CHECK(r.ok);
    if (!r.ok && r.error) std::fprintf(stderr, "http error: %s\n", r.error->message.c_str());
    CHECK_EQ(r.remotePath, std::string("/Books/Http/book one.epub"));
    CHECK(dev.file("/Books/Http/book one.epub") == f.bytes);
    CHECK(rec.saw(TransferState::Uploading));
    CHECK(rec.saw(TransferState::Verifying));
    CHECK(rec.last() == TransferState::Complete);

    // Duplicate: "already exists" -> /delete -> retry once.
    int deletesBefore = dev.deletes;
    f.bytes = makeFile("x", 5000).bytes;
    r = t.send(p, f, nullptr);
    CHECK(r.ok);
    CHECK_EQ(dev.deletes, deletesBefore + 1);
    CHECK(dev.file("/Books/Http/book one.epub") == f.bytes);

    // Delete refused -> Exists error.
    dev.allowDelete = false;
    r = t.send(p, f, nullptr);
    dev.allowDelete = true;
    CHECK(!r.ok);
    CHECK(r.error && r.error->code == TransferErrorCode::Exists);
}

static void testWebSocket(FakeDevice& dev) {
    using namespace ks::net;
    auto& t = transport(TransferMethod::CrossPointWs);
    CHECK(t.isSupported());
    setWsPort(dev.port());
    DeviceProfile p = fakeProfile(dev, TransferMethod::CrossPointWs);
    p.targetFolder = "/Books/Ws/";
    auto tc = t.testConnection(p);
    CHECK(tc.ok);
    if (!tc.ok && tc.error) std::fprintf(stderr, "ws test error: %s\n", tc.error->message.c_str());

    OutputFile f = makeFile("ws book.epub", 100000);
    Recorder rec;
    auto r = t.send(p, f, rec.fn());
    CHECK(r.ok);
    if (!r.ok && r.error) std::fprintf(stderr, "ws error: %s\n", r.error->message.c_str());
    CHECK(dev.file("/Books/Ws/ws book.epub") == f.bytes);
    CHECK(rec.saw(TransferState::Uploading));
    CHECK(rec.saw(TransferState::Verifying));
    CHECK(rec.last() == TransferState::Complete);

    OutputFile bad = makeFile("a:b.epub", 10);
    r = t.send(p, bad, nullptr);
    CHECK(!r.ok);
    setWsPort(81);
    CHECK_EQ(wsPort(), 81);
}

static void testFolderAndBle() {
    using namespace ks::net;
    std::string root = joinPath(dataDir(), "usb-drive");
    makeDirs(root);
    DeviceProfile p = defaultProfile(FirmwareType::CrossPoint);
    p.method = TransferMethod::Folder;
    p.host = root;
    p.targetFolder = "/Books/Inbox/";
    auto& t = transport(TransferMethod::Folder);
    CHECK(t.testConnection(p).ok);
    OutputFile f = makeFile("usb.epub", 200000);
    Recorder rec;
    auto r = t.send(p, f, rec.fn());
    CHECK(r.ok);
    CHECK(readFile(joinPath(root, "Books/Inbox/usb.epub")) == f.bytes);
    CHECK(rec.saw(TransferState::Verifying));
    CHECK(rec.last() == TransferState::Complete);
    p.host = joinPath(root, "missing");
    r = t.send(p, f, nullptr);
    CHECK(!r.ok && r.error && r.error->code == TransferErrorCode::NotFound);

    auto& ble = transport(TransferMethod::Ble);
    p.targetFolder = "/Books/K-rosspoint/";
    Recorder br;
    auto t0 = std::chrono::steady_clock::now();
    r = ble.send(p, makeFile("ble.epub", 10000), br.fn());
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r.ok);
    CHECK_EQ(r.remotePath, std::string("/Books/K-rosspoint/ble.epub"));
    CHECK(ms >= 1000 && ms < 3000);
    int uploads = 0;
    for (auto& e : br.events)
        if (e.state == TransferState::Uploading) ++uploads;
    CHECK_EQ(uploads, 16);
    CHECK(br.saw(TransferState::Verifying));
    CHECK(br.last() == TransferState::Complete);

    // Cancel in the middle.
    bool cancel = false;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        cancel = true;
    });
    r = ble.send(p, makeFile("ble.epub", 10000), nullptr, &cancel);
    stopper.join();
    CHECK(!r.ok && r.error && r.error->code == TransferErrorCode::Cancelled);
}

static void testNetworkError() {
    using namespace ks::net;
    DeviceProfile p = defaultProfile(FirmwareType::CrossPoint);
    p.host = "127.0.0.1";
    p.port = 1;   // nothing listens here
    Recorder rec;
    auto r = transport(TransferMethod::CrossPointHttp).send(p, makeFile("a.epub", 10), rec.fn());
    CHECK(!r.ok);
    CHECK(r.error && r.error->code == TransferErrorCode::Network);
    CHECK(rec.last() == TransferState::Failed);
}

static void testHelper(FakeDevice& dev) {
    using namespace ks::net;
    HelperServer hs;
    CHECK(!hs.running());
    CHECK(hs.start(0, AppSettings{}.helperAllowedOrigins));
    CHECK(hs.running());
    int port = hs.port();
    CHECK(port > 0);

    // A second server on the same port fails with a message.
    HelperServer hs2;
    CHECK(!hs2.start(port, {}));
    CHECK(!hs2.lastError().empty());

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(5, 0);
    cli.set_read_timeout(30, 0);
    auto st = cli.Get("/status");
    CHECK(st && st->status == 200);
    if (st) {
        auto j = json::parse(st->body, nullptr, false);
        CHECK(j.is_object() && j.value("ok", false) && j.value("version", std::string()) == "desktop-1.0");
    }

    // CORS preflight.
    auto pre = cli.Options("/send", {{"Origin", "http://localhost:5173"}, {"Access-Control-Request-Private-Network", "true"}});
    CHECK(pre && pre->status == 204);
    if (pre) {
        CHECK_EQ(pre->get_header_value("Access-Control-Allow-Origin"), std::string("http://localhost:5173"));
        CHECK_EQ(pre->get_header_value("Access-Control-Allow-Private-Network"), std::string("true"));
    }
    auto evil = cli.Get("/status", {{"Origin", "https://evil.example"}});
    CHECK(evil && evil->status == 403);
    if (evil) CHECK(!evil->has_header("Access-Control-Allow-Origin"));
    auto nul = cli.Get("/status", {{"Origin", "null"}});
    CHECK(nul && nul->status == 403);
    auto good = cli.Get("/status", {{"Origin", "https://k-engine.org"}});
    CHECK(good && good->status == 200);
    if (good) CHECK_EQ(good->get_header_value("Access-Control-Allow-Origin"), std::string("https://k-engine.org"));
    auto rebind = cli.Get("/status", {{"Host", "evil.example:8134"}});
    CHECK(rebind && rebind->status == 403);

    // POST /send, WebDAV.
    std::string bytes = makeFile("h", 50000).bytes;
    httplib::MultipartFormDataItems items = {
        {"file", bytes, "helper book.epub", "application/epub+zip"},
        {"host", "127.0.0.1", "", ""},
        {"port", std::to_string(dev.port()), "", ""},
        {"folder", "/Books/Helper/", "", ""},
        {"method", "webdav", "", ""},
    };
    auto sr = cli.Post("/send", {{"Origin", "http://127.0.0.1:3000"}}, items);
    CHECK(sr && sr->status == 200);
    if (sr) {
        auto j = json::parse(sr->body, nullptr, false);
        CHECK(j.is_object() && j.value("ok", false));
        CHECK(j.is_object() && j.value("remotePath", std::string()) == "/Books/Helper/helper book.epub");
        CHECK(j.is_object() && j.value("bytes", 0) == 50000);
        CHECK_EQ(sr->get_header_value("Access-Control-Allow-Origin"), std::string("http://127.0.0.1:3000"));
    }
    CHECK(dev.file("/Books/Helper/helper book.epub") == bytes);

    // POST /send, CrossPoint HTTP (twice: the second one replaces the file).
    items[4].content = "crosspoint-http";
    items[3].content = "/Books/HelperHttp";
    auto hr = cli.Post("/send", items);
    CHECK(hr && hr->status == 200);
    hr = cli.Post("/send", items);
    CHECK(hr && hr->status == 200);
    CHECK(dev.file("/Books/HelperHttp/helper book.epub") == bytes);

    // Bad input.
    items[1].content = "http://bad host";
    auto bad = cli.Post("/send", items);
    CHECK(bad && bad->status == 400);
    if (bad) {
        auto j = json::parse(bad->body, nullptr, false);
        CHECK(j.is_object() && !j.value("ok", true) && !j.value("error", std::string()).empty());
    }
    items[1].content = "127.0.0.1";
    items[4].content = "carrier-pigeon";
    bad = cli.Post("/send", items);
    CHECK(bad && bad->status == 400);
    if (bad) CHECK(json::parse(bad->body, nullptr, false).value("code", std::string()) == "unsupported");

    // Device down -> 502 with a code.
    items[4].content = "webdav";
    items[2].content = "1";
    auto down = cli.Post("/send", items);
    CHECK(down && down->status == 502);
    if (down) CHECK(json::parse(down->body, nullptr, false).value("code", std::string()) == "network");

    // POST /test.
    json tb = {{"host", "127.0.0.1"}, {"port", dev.port()}};
    auto tr = cli.Post("/test", tb.dump(), "application/json");
    CHECK(tr && tr->status == 200);
    if (tr) {
        auto j = json::parse(tr->body, nullptr, false);
        CHECK(j.is_object() && j.value("ok", false));
        CHECK(j.is_object() && j.contains("info") && j["info"].value("version", std::string()) == "1.2.0-fake");
        CHECK(j.is_object() && j.contains("latencyMs"));
    }

    auto nf = cli.Get("/nothing");
    CHECK(nf && nf->status == 404);

    auto t0 = std::chrono::steady_clock::now();
    hs.stop();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 2000);
    CHECK(!hs.running());
    CHECK_EQ(hs.port(), 0);
}

// Folder browser: list and create folders over CrossPoint HTTP and WebDAV.
static void testRemoteFolder(FakeDevice& dev) {
    for (TransferMethod m : {TransferMethod::CrossPointHttp, TransferMethod::WebDav}) {
        DeviceProfile p = fakeProfile(dev, m);
        const std::string top = m == TransferMethod::WebDav ? "DavTop" : "HttpTop";
        CHECK(net::canBrowseRemote(m));
        net::makeRemoteFolder(p, "/", top);
        net::makeRemoteFolder(p, "/" + top + "/", "K-rosspoint");
        net::makeRemoteFolder(p, "/" + top, "Alpha");
        {
            std::lock_guard<std::mutex> lk(dev.m);
            CHECK(dev.dirs.count("/" + top + "/K-rosspoint") == 1);
            CHECK(dev.dirs.count("/" + top + "/Alpha") == 1);
            dev.files["/" + top + "/note.epub"] = std::string(1234, 'x');
        }
        auto list = net::listRemoteFolder(p, "/" + top + "/");
        CHECK_EQ(list.size(), size_t(3));
        if (list.size() == 3) {
            CHECK(list[0].isDirectory && list[0].name == "Alpha");      // folders first, sorted
            CHECK(list[1].isDirectory && list[1].name == "K-rosspoint");
            CHECK(!list[2].isDirectory && list[2].name == "note.epub");
            CHECK_EQ(list[2].size, uint64_t(1234));
        }
        auto root = net::listRemoteFolder(p, "/");
        bool found = false;
        for (auto& e : root) found |= e.isDirectory && e.name == top;
        CHECK(found);
        bool threw = false;
        try {
            net::makeRemoteFolder(p, "/", "bad/name");
        } catch (const TransferError& e) {
            threw = e.code == TransferErrorCode::Device;
        }
        CHECK(threw);
    }
    // WebDAV MKCOL on an existing folder answers 405. That is not an error.
    DeviceProfile dav = fakeProfile(dev, TransferMethod::WebDav);
    bool ok = true;
    try {
        net::makeRemoteFolder(dav, "/", "DavTop");
    } catch (const TransferError&) {
        ok = false;
    }
    CHECK(ok);
    CHECK(dev.propfinds >= 2);
    // A missing WebDAV folder gives NotFound.
    TransferErrorCode code = TransferErrorCode::Unknown;
    try {
        net::listRemoteFolder(dav, "/Nope/");
    } catch (const TransferError& e) {
        code = e.code;
    }
    CHECK(code == TransferErrorCode::NotFound);
    // Other methods cannot list folders.
    DeviceProfile ws = fakeProfile(dev, TransferMethod::CrossPointWs);
    CHECK(!net::canBrowseRemote(TransferMethod::Folder));
    code = TransferErrorCode::Unknown;
    try {
        net::listRemoteFolder(ws, "/");
    } catch (const TransferError& e) {
        code = e.code;
    }
    CHECK(code == TransferErrorCode::Unsupported);
    code = TransferErrorCode::Unknown;
    try {
        net::makeRemoteFolder(ws, "/", "x");
    } catch (const TransferError& e) {
        code = e.code;
    }
    CHECK(code == TransferErrorCode::Unsupported);

    // The parser: the D: prefix, an encoded href, the folder itself skipped.
    std::string xml = R"(<?xml version="1.0"?><D:multistatus xmlns:D="DAV:">
<D:response><D:href>/Books/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop></D:propstat></D:response>
<D:response><D:href>/Books/My%20Folder/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop></D:propstat></D:response>
<D:response><D:href>http://x/Books/a.epub</D:href><D:propstat><D:prop><D:resourcetype/><D:getcontentlength>42</D:getcontentlength></D:prop></D:propstat></D:response>
</D:multistatus>)";
    auto pe = net::parsePropfind(xml, "/Books/");
    CHECK_EQ(pe.size(), size_t(2));
    if (pe.size() == 2) {
        CHECK(pe[0].name == "My Folder" && pe[0].isDirectory);
        CHECK(pe[1].name == "a.epub" && !pe[1].isDirectory && pe[1].size == 42);
    }
    CHECK(net::parsePropfind("not xml", "/").empty());
}

// Runs one test section and prints its time.
#define TIMED(call)                                                                               \
    do {                                                                                          \
        auto t0_ = std::chrono::steady_clock::now();                                              \
        call;                                                                                     \
        std::printf("%-28s %lld ms\n", #call,                                                     \
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>( \
                                               std::chrono::steady_clock::now() - t0_)            \
                                               .count()));                                        \
    } while (0)

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    if (!std::getenv("KS_DATA_DIR")) {
        std::fprintf(stderr, "KS_DATA_DIR is not set. Run this test through ctest.\n");
        return 1;
    }
    TIMED(testPure());
    TIMED(testProfiles());
    TIMED(testSettings());
    TIMED(testLibrary());

    FakeDevice dev;
    CHECK(dev.start());
    TIMED(testWebDav(dev));
    TIMED(testCrossPointHttp(dev));
    TIMED(testWebSocket(dev));
    TIMED(testHelper(dev));
    TIMED(testRemoteFolder(dev));
    dev.stop();

    TIMED(testFolderAndBle());
    TIMED(testNetworkError());
    TEST_MAIN_END();
}
