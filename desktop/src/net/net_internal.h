// Internal helpers for src/net. Not part of the public API.
#pragma once

#include "net/net.h"

#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ks::net::detail {

constexpr long kConnectTimeoutSec = 5;     // connect timeout for every request
constexpr long kSmallTimeoutSec = 15;      // total timeout for small requests
constexpr const char* kUserAgent = "k-rosspointSender/0.1";

// A transfer error as a C++ exception. Only used inside src/net.
struct NetError : std::exception {
    TransferError err;
    NetError(TransferErrorCode c, std::string m) { err.code = c; err.message = std::move(m); }
    explicit NetError(TransferError e) : err(std::move(e)) {}
    const char* what() const noexcept override { return err.message.c_str(); }
};

TransferError makeError(TransferErrorCode c, const std::string& m);
NetError cancelled();

// True when *cancel is set. Reads through volatile so the compiler reads it each time.
inline bool isCancelled(const bool* cancel) {
    return cancel && *static_cast<const volatile bool*>(cancel);
}

// Sleeps ms milliseconds in small steps. Throws cancelled() when the flag is set.
void sleepCancellable(int ms, const bool* cancel);

// Calls the progress callback. Ignores exceptions from the callback.
class Emitter {
public:
    explicit Emitter(const ProgressFn& fn) : fn_(fn) {}
    void operator()(TransferState s, const std::string& msg = {}, double fraction = -1,
                    uint64_t sent = 0, uint64_t total = 0) const;
private:
    const ProgressFn& fn_;
};

// Runs a send body. The body returns the remote path or throws NetError.
// Adds timing, the Complete event, and the Failed event on error.
TransferResult runSend(const OutputFile& file, const ProgressFn& onProgress,
                       const std::function<std::string()>& body);

// ---- HTTP with libcurl

struct HttpRequest {
    std::string method = "GET";          // GET POST PUT HEAD OPTIONS MKCOL ...
    std::string url;
    std::vector<std::string> headers;    // "Name: value"
    std::string username, password;      // Basic auth when username is set
    const std::string* body = nullptr;   // request body (POST form or PUT upload)
    bool upload = false;                 // PUT body with CURLOPT_UPLOAD + read callback
    // Multipart file part (POST /upload): field name "file".
    const std::string* fileBytes = nullptr;
    std::string fileName, fileMime;
    long timeoutSec = kSmallTimeoutSec;  // 0 = no total timeout (uploads)
    const bool* cancel = nullptr;
    std::function<void(uint64_t now, uint64_t total)> onUpload;   // upload progress
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;   // lowercase names
    std::string header(const std::string& lowerName) const;
};

// Sends one request. Returns any HTTP status. Throws NetError on a network
// error, a timeout or a cancel.
HttpResponse httpRequest(const HttpRequest& r, const std::string& hostForMessages);

std::string baseUrl(const DeviceProfile& p);         // http://host:port
std::string encodePath(const std::string& path);     // percent-encodes each segment, keeps '/'
std::string encodeComponent(const std::string& s);   // like encodeURIComponent
std::string formEncode(const std::vector<std::pair<std::string, std::string>>& kv);

NetError httpError(long status, const std::string& what, const std::string& body = {});
TransferErrorCode statusCode(long status);

// The error for a failed connection to host.
NetError networkError(const std::string& host);

// CrossPoint HTTP send, used by the transport and by the helper server.
ConnectionTestResult crosspointStatus(const DeviceProfile& p);

// Milliseconds since t0.
inline int msSince(std::chrono::steady_clock::time_point t0) {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count());
}

// One-time global init of libcurl (thread safe).
void ensureCurl();

// LAN limitation text shared by the LAN transports.
std::vector<std::string> lanLimitations();

} // namespace ks::net::detail
