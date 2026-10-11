// The "local helper app" for the web version, with cpp-httplib.
// Same endpoints and rules as helper/helper.py, so the web app works unchanged:
// - Listens only on 127.0.0.1.
// - The Host header must name the loopback interface (blocks DNS rebinding).
// - A browser origin must be in the allow-list. Other origins get 403.
//   A request with no Origin header is not from a browser page. It is allowed.
// - The maximum upload is 200 MiB. Files stay in memory. Nothing is logged.
#include "net/net_internal.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <mutex>
#include <regex>
#include <thread>

using nlohmann::json;

namespace ks::net {

namespace {

constexpr const char* kVersion = "desktop-1.1";
constexpr size_t kMaxUpload = 200u * 1024u * 1024u;

struct HelperFail {
    TransferErrorCode code;
    std::string message;
    int status;
};

bool isLoopbackEntry(const std::string& e) { return e == "http://localhost" || e == "http://127.0.0.1"; }

bool hostHeaderAllowed(const std::string& hostHeader) {
    std::string h = toLower(trim(hostHeader));
    if (h.empty()) return false;
    std::string name;
    if (h[0] == '[') {
        auto close = h.find(']');
        if (close == std::string::npos) return false;
        name = h.substr(1, close - 1);
    } else {
        name = h.substr(0, h.find(':'));
    }
    return name == "127.0.0.1" || name == "localhost" || name == "::1";
}

std::string field(const httplib::Request& req, const std::string& key, const std::string& def = {}) {
    if (req.has_file(key)) return req.get_file_value(key).content;
    if (req.has_param(key)) return req.get_param_value(key);
    return def;
}

std::string safeFilename(const std::string& in) {
    std::string name = in;
    auto slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string out;
    for (unsigned char c : name) {
        if (c < 0x20 || std::string("<>:\"|?*").find(static_cast<char>(c)) != std::string::npos) continue;
        out += static_cast<char>(c);
    }
    size_t a = out.find_first_not_of(" .");
    size_t b = out.find_last_not_of(" .");
    out = a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
    if (out.empty()) throw HelperFail{TransferErrorCode::Unknown, "The file has no usable name.", 400};
    return out;
}

int parsePort(const std::string& s) {
    std::string t = trim(s);
    if (t.empty() || t.size() > 5 || t.find_first_not_of("0123456789") != std::string::npos)
        throw HelperFail{TransferErrorCode::Unknown, "The port must be a number.", 400};
    int port = std::stoi(t);
    if (port < 1 || port > 65535) throw HelperFail{TransferErrorCode::Unknown, "The port must be from 1 to 65535.", 400};
    return port;
}

void sendJson(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

void sendFail(httplib::Response& res, const HelperFail& f) {
    sendJson(res, f.status, {{"ok", false}, {"error", f.message}, {"code", toString(f.code)}});
}

} // namespace

bool originAllowed(const std::string& origin, const std::vector<std::string>& allowed) {
    if (origin.empty() || origin == "null") return false;
    static const std::regex portSuffix(R"(^:\d{1,5}$)");
    for (std::string e : allowed) {
        while (!e.empty() && e.back() == '/') e.pop_back();
        if (e.empty() || e == "null") continue;
        if (origin == e) return true;
        if (isLoopbackEntry(e) && origin.size() > e.size() && origin.compare(0, e.size(), e) == 0 &&
            std::regex_match(origin.substr(e.size()), portSuffix))
            return true;
    }
    return false;
}

struct HelperServer::Impl {
    std::unique_ptr<httplib::Server> svr;
    std::thread thread;
    std::vector<std::string> origins;
    std::atomic<bool> running{false};
    std::atomic<int> boundPort{0};
    mutable std::mutex m;
    std::string lastError;

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lk(m);
        lastError = e;
    }

    // Origin check for a request. No Origin header = not a browser = allowed.
    bool requestOriginOk(const httplib::Request& req) const {
        if (!req.has_header("Origin")) return true;
        return originAllowed(req.get_header_value("Origin"), origins);
    }

    void addCors(const httplib::Request& req, httplib::Response& res) const {
        if (!req.has_header("Origin")) return;
        std::string origin = req.get_header_value("Origin");
        if (!originAllowed(origin, origins)) return;
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Vary", "Origin");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.set_header("Access-Control-Max-Age", "600");
        // Chrome Private Network Access preflight.
        res.set_header("Access-Control-Allow-Private-Network", "true");
    }

    void handleSend(const httplib::Request& req, httplib::Response& res) {
        if (!req.is_multipart_form_data())
            throw HelperFail{TransferErrorCode::Unknown, "Send multipart/form-data.", 400};
        if (!req.has_file("file") || req.get_file_value("file").filename.empty())
            throw HelperFail{TransferErrorCode::Unknown, "The \"file\" field is missing.", 400};
        auto filePart = req.get_file_value("file");
        std::string name = safeFilename(filePart.filename);
        std::string host = trim(field(req, "host"));
        if (!isValidHost(host))
            throw HelperFail{TransferErrorCode::Unknown, "The host must be a host name or an IPv4 address.", 400};
        int port = parsePort(field(req, "port", "80"));
        std::string folder = field(req, "folder", "/Books/K-rosspoint/");
        if (folder.empty() || folder[0] != '/')
            throw HelperFail{TransferErrorCode::Unknown, "The folder must start with \"/\".", 400};
        std::string method = field(req, "method", "webdav");

        DeviceProfile p;
        p.name = "helper";
        p.host = host;
        p.port = port;
        p.targetFolder = folder;
        if (method == "webdav") {
            p.method = TransferMethod::WebDav;
            p.username = field(req, "username");
            p.password = field(req, "password");
        } else if (method == "crosspoint-http") {
            p.method = TransferMethod::CrossPointHttp;
        } else {
            throw HelperFail{TransferErrorCode::Unsupported,
                             "Unknown method \"" + method + "\". Use webdav or crosspoint-http.", 400};
        }

        OutputFile f;
        f.name = name;
        f.bytes = filePart.content;
        f.mime = toLower(name).size() > 5 && toLower(name).compare(name.size() - 5, 5, ".epub") == 0
                     ? "application/epub+zip"
                     : "application/octet-stream";
        TransferResult r = transport(p.method).send(p, f, nullptr, nullptr);
        if (!r.ok) {
            TransferError e = r.error.value_or(detail::makeError(TransferErrorCode::Unknown, "The transfer failed."));
            throw HelperFail{e.code, e.message, 502};
        }
        sendJson(res, 200, {{"ok", true}, {"remotePath", r.remotePath}, {"bytes", r.bytes}});
    }

    void handleTest(const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body.empty() ? std::string("{}") : req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object())
            throw HelperFail{TransferErrorCode::Unknown, "Send JSON {host, port}.", 400};
        std::string host = body.contains("host") && body["host"].is_string() ? trim(body["host"].get<std::string>()) : "";
        if (!isValidHost(host))
            throw HelperFail{TransferErrorCode::Unknown, "The host must be a host name or an IPv4 address.", 400};
        int port = 80;
        if (body.contains("port")) {
            const auto& pv = body["port"];
            if (pv.is_number_integer()) port = pv.get<int>();
            else if (pv.is_string()) port = parsePort(pv.get<std::string>());
            else throw HelperFail{TransferErrorCode::Unknown, "The port must be a number.", 400};
            if (port < 1 || port > 65535) throw HelperFail{TransferErrorCode::Unknown, "The port must be from 1 to 65535.", 400};
        }
        DeviceProfile p;
        p.host = host;
        p.port = port;
        auto r = detail::crosspointStatus(p);
        if (!r.ok) {
            TransferError e = r.error.value_or(detail::makeError(TransferErrorCode::Unknown, "The status request failed."));
            throw HelperFail{e.code, e.message, 502};
        }
        json info = json::parse(r.info, nullptr, false);
        if (info.is_discarded()) info = {{"raw", r.info.substr(0, 200)}};
        sendJson(res, 200, {{"ok", true}, {"info", info}, {"latencyMs", r.latencyMs}});
    }

    void handleDiscover(httplib::Response& res) {
        json list = json::array();
        for (auto& d : discoverCrossPoint())
            list.push_back({{"ip", d.ip}, {"hostname", d.hostname}, {"wsPort", d.wsPort}, {"raw", d.raw}});
        sendJson(res, 200, {{"ok", true}, {"devices", list}});
    }

    // Runs a handler. Turns every error into a JSON answer. httplib is built
    // without exception support, so no exception may leave a handler.
    template <typename F>
    void guarded(httplib::Response& res, F&& fn) {
        try {
            fn();
        } catch (const HelperFail& f) {
            sendFail(res, f);
        } catch (const std::exception& e) {
            sendFail(res, {TransferErrorCode::Unknown, std::string("Helper error: ") + e.what(), 500});
        } catch (...) {
            sendFail(res, {TransferErrorCode::Unknown, "Helper error.", 500});
        }
    }

    void setup() {
        // Own the port alone. The httplib default lets a second program bind
        // the same port (SO_REUSEADDR on Windows, SO_REUSEPORT on POSIX).
        svr->set_socket_options([](socket_t sock) {
            int opt = 1;
#ifdef _WIN32
            setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const void*>(&opt), sizeof(opt));
#endif
        });
        svr->set_payload_max_length(kMaxUpload);
        svr->set_read_timeout(30, 0);
        svr->set_write_timeout(30, 0);

        svr->set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (!hostHeaderAllowed(req.get_header_value("Host"))) {
                sendJson(res, 403, {{"ok", false}, {"error", "Bad Host header."}, {"code", "auth"}});
                return httplib::Server::HandlerResponse::Handled;
            }
            if (!requestOriginOk(req)) {
                if (req.method == "OPTIONS") {
                    res.status = 403;
                } else {
                    sendJson(res, 403, {{"ok", false}, {"error", "This origin may not use the helper."}, {"code", "auth"}});
                }
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        svr->set_post_routing_handler([this](const httplib::Request& req, httplib::Response& res) { addCors(req, res); });

        svr->Options(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
        svr->Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            guarded(res, [&] { sendJson(res, 200, {{"ok", true}, {"version", kVersion}}); });
        });
        svr->Get("/discover", [this](const httplib::Request&, httplib::Response& res) {
            guarded(res, [&] { handleDiscover(res); });
        });
        svr->Post("/send", [this](const httplib::Request& req, httplib::Response& res) {
            guarded(res, [&] { handleSend(req, res); });
        });
        svr->Post("/test", [this](const httplib::Request& req, httplib::Response& res) {
            guarded(res, [&] { handleTest(req, res); });
        });
        svr->set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (res.status == 404 && res.body.empty())
                sendJson(res, 404, {{"ok", false}, {"error", "Not found."}, {"code", "not-found"}});
            else if (res.status == 413)
                sendJson(res, 413, {{"ok", false}, {"error", "The request is too large (limit 200 MiB)."}, {"code", "unknown"}});
        });
    }
};

HelperServer::HelperServer() : impl_(std::make_unique<Impl>()) {}

HelperServer::~HelperServer() { stop(); }

bool HelperServer::start(int port, const std::vector<std::string>& allowedOrigins) {
    stop();
    impl_->setError("");
    impl_->origins = allowedOrigins;
    impl_->svr = std::make_unique<httplib::Server>();
    impl_->setup();
    int bound = 0;
    if (port == 0) {
        bound = impl_->svr->bind_to_any_port("127.0.0.1");
        if (bound <= 0) bound = 0;
    } else if (port > 0 && port < 65536 && impl_->svr->bind_to_port("127.0.0.1", port)) {
        bound = port;
    }
    if (bound == 0) {
        impl_->setError("Could not listen on 127.0.0.1:" + std::to_string(port) +
                        ". Another program (or another helper) can use this port.");
        impl_->svr.reset();
        return false;
    }
    impl_->boundPort = bound;
    impl_->running = true;
    httplib::Server* svr = impl_->svr.get();
    Impl* impl = impl_.get();
    impl_->thread = std::thread([svr, impl] {
        if (!svr->listen_after_bind()) impl->setError("The helper server stopped with an error.");
        impl->running = false;
    });
    return true;
}

void HelperServer::stop() {
    if (!impl_ || !impl_->svr) return;
    impl_->svr->stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->svr.reset();
    impl_->running = false;
    impl_->boundPort = 0;
}

bool HelperServer::running() const { return impl_ && impl_->running.load(); }

std::string HelperServer::lastError() const {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->lastError;
}

int HelperServer::port() const { return impl_ ? impl_->boundPort.load() : 0; }

} // namespace ks::net
