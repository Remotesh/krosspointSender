// WebDAV transport. CrossPoint serves WebDAV at the root on port 80.
// Steps: OPTIONS (connecting), MKCOL for each folder segment, PUT with upload
// progress (uploading), HEAD to compare Content-Length (verifying).
// PUT overwrites. The device returns 201 for a new file and 204 for a replaced file.
#include "net/net_internal.h"

#include <nlohmann/json.hpp>

namespace ks::net::detail {

namespace {

HttpRequest baseReq(const DeviceProfile& p, const std::string& method, const std::string& url, const bool* cancel) {
    HttpRequest r;
    r.method = method;
    r.url = url;
    r.username = p.username;
    r.password = p.password;
    r.cancel = cancel;
    return r;
}

} // namespace

class WebDavTransport final : public Transport {
public:
    TransferMethod method() const override { return TransferMethod::WebDav; }
    bool isSupported() const override { return true; }
    std::vector<std::string> limitations() const override {
        auto out = lanLimitations();
        out.push_back("WebDAV replaces a file with the same name.");
        return out;
    }

    ConnectionTestResult testConnection(const DeviceProfile& p) override {
        ConnectionTestResult res;
        auto t0 = std::chrono::steady_clock::now();
        try {
            auto r = httpRequest(baseReq(p, "OPTIONS", baseUrl(p) + "/", nullptr), p.host);
            if (r.status < 200 || r.status >= 300) throw httpError(r.status, "The WebDAV check");
            nlohmann::json info = {{"status", r.status}, {"dav", r.header("dav")}, {"allow", r.header("allow")}};
            res.ok = true;
            res.info = info.dump();
        } catch (const NetError& e) {
            res.error = e.err;
        }
        res.latencyMs = msSince(t0);
        return res;
    }

    TransferResult send(const DeviceProfile& p, const OutputFile& file, const ProgressFn& onProgress,
                        const bool* cancel) override {
        return runSend(file, onProgress, [&]() -> std::string {
            Emitter emit(onProgress);
            const std::string base = baseUrl(p);
            const uint64_t total = file.bytes.size();

            // 1. Connecting: OPTIONS.
            emit(TransferState::Connecting, "Connecting to " + p.host + ":" + std::to_string(p.port));
            auto opt = httpRequest(baseReq(p, "OPTIONS", base + "/", cancel), p.host);
            if (opt.status == 401 || opt.status == 403) throw httpError(opt.status, "The connection");

            // 2. MKCOL each folder segment. 405/409/301 mean the folder exists.
            std::string path = "/";
            for (const auto& seg : folderSegments(p.targetFolder)) {
                path += seg + "/";
                auto r = httpRequest(baseReq(p, "MKCOL", base + encodePath(path), cancel), p.host);
                if ((r.status >= 200 && r.status < 300) || r.status == 405 || r.status == 409 || r.status == 301)
                    continue;
                throw httpError(r.status, "Creating the folder " + path, r.body);
            }

            // 3. Uploading: PUT with progress.
            const std::string remote = remoteFilePath(p.targetFolder, file.name);
            const std::string url = base + encodePath(remote);
            emit(TransferState::Uploading, "Uploading " + file.name, 0, 0, total);
            HttpRequest put = baseReq(p, "PUT", url, cancel);
            put.upload = true;
            put.body = &file.bytes;
            put.timeoutSec = 0;
            put.headers.push_back("Content-Type: " + (file.mime.empty() ? std::string("application/octet-stream") : file.mime));
            put.onUpload = [&](uint64_t now, uint64_t t) {
                emit(TransferState::Uploading, {}, t ? static_cast<double>(now) / static_cast<double>(t) : -1, now,
                     t ? t : total);
            };
            auto pr = httpRequest(put, p.host);
            if (pr.status < 200 || pr.status >= 300) throw httpError(pr.status, "The upload", pr.body);

            // 4. Verifying: HEAD. Compare Content-Length when the server returns it.
            emit(TransferState::Verifying, "Checking the file on the device", 1, total, total);
            auto head = httpRequest(baseReq(p, "HEAD", url, cancel), p.host);
            if (head.status < 200 || head.status >= 300) throw httpError(head.status, "The check after upload");
            std::string len = head.header("content-length");
            if (!len.empty()) {
                bool digits = len.find_first_not_of("0123456789") == std::string::npos;
                if (digits && std::stoull(len) != total) {
                    throw NetError(TransferErrorCode::Device, "The device reports " + len + " bytes, but the file has " +
                                                                  std::to_string(total) + " bytes. Send the file again.");
                }
            }
            return remote;
        });
    }
};

Transport& webdavTransport() {
    static WebDavTransport t;
    return t;
}

} // namespace ks::net::detail
