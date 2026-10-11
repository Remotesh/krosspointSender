// CrossPoint native HTTP upload (port 80, File Transfer screen).
// Steps: POST /mkdir for each folder segment (errors are ignored), then
// POST /upload?path=<dir> multipart field "file" with upload progress.
// When the device answers 400 "File already exists", POST /delete with
// paths=["<path>"] and try again one time. Verify with GET /api/files?path=<dir>.
#include "net/net_internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

namespace ks::net::detail {

namespace {

bool saysExists(const HttpResponse& r) {
    return r.status == 400 && toLower(r.body).find("already exists") != std::string::npos;
}

} // namespace

ConnectionTestResult crosspointStatus(const DeviceProfile& p) {
    ConnectionTestResult res;
    auto t0 = std::chrono::steady_clock::now();
    try {
        HttpRequest q;
        q.url = baseUrl(p) + "/api/status";
        auto r = httpRequest(q, p.host);
        if (r.status < 200 || r.status >= 300) throw httpError(r.status, "The status request");
        auto j = nlohmann::json::parse(r.body, nullptr, false);
        if (j.is_discarded() || !j.is_object())
            throw NetError(TransferErrorCode::Device,
                           "The device answered, but /api/status did not return JSON. Is this a CrossPoint device?");
        res.ok = true;
        res.info = j.dump();
    } catch (const NetError& e) {
        res.error = e.err;
    }
    res.latencyMs = msSince(t0);
    return res;
}

class CrossPointHttpTransport final : public Transport {
public:
    TransferMethod method() const override { return TransferMethod::CrossPointHttp; }
    bool isSupported() const override { return true; }
    std::vector<std::string> limitations() const override {
        auto out = lanLimitations();
        out.push_back("The device rejects a file with the same name. The app deletes the old file and tries again one time.");
        return out;
    }

    ConnectionTestResult testConnection(const DeviceProfile& p) override { return crosspointStatus(p); }

    TransferResult send(const DeviceProfile& p, const OutputFile& file, const ProgressFn& onProgress,
                        const bool* cancel) override {
        return runSend(file, onProgress, [&]() -> std::string {
            Emitter emit(onProgress);
            const std::string base = baseUrl(p);
            const uint64_t total = file.bytes.size();
            const std::string dir = folderNoTrailingSlash(p.targetFolder);
            const std::string remote = remoteFilePath(p.targetFolder, file.name);
            const std::string formType = "Content-Type: application/x-www-form-urlencoded";

            // 1. Connecting: create each folder segment. CrossPoint needs the folder to exist.
            emit(TransferState::Connecting, "Connecting to " + p.host + ":" + std::to_string(p.port));
            std::string parent = "/";
            for (const auto& seg : folderSegments(p.targetFolder)) {
                std::string form = formEncode({{"name", seg}, {"path", parent}});
                HttpRequest q;
                q.method = "POST";
                q.url = base + "/mkdir";
                q.body = &form;
                q.headers.push_back(formType);
                q.cancel = cancel;
                try {
                    httpRequest(q, p.host);
                } catch (const NetError& e) {
                    // Ignore HTTP errors from mkdir. The folder can exist. A real failure shows in
                    // the upload. Stop on a cancel, and when the device does not answer at all:
                    // the upload cannot work then, and each try costs a connect timeout.
                    if (e.err.code == TransferErrorCode::Cancelled || e.err.code == TransferErrorCode::Network ||
                        e.err.code == TransferErrorCode::NotFound)
                        throw;
                }
                parent = parent == "/" ? "/" + seg : parent + "/" + seg;
            }

            // 2. Uploading: multipart POST. Try again one time after delete when the file exists.
            auto upload = [&]() {
                emit(TransferState::Uploading, "Uploading " + file.name, 0, 0, total);
                HttpRequest q;
                q.method = "POST";
                q.url = base + "/upload?path=" + encodeComponent(dir);
                q.fileBytes = &file.bytes;
                q.fileName = file.name;
                q.fileMime = file.mime;
                q.timeoutSec = 0;
                q.cancel = cancel;
                q.onUpload = [&](uint64_t now, uint64_t t) {
                    // The multipart body is a little larger than the file. Clamp to the file size.
                    double frac = t ? std::min(1.0, static_cast<double>(now) / static_cast<double>(t)) : -1;
                    uint64_t sent = frac >= 0 ? static_cast<uint64_t>(std::llround(frac * static_cast<double>(total))) : 0;
                    emit(TransferState::Uploading, {}, frac, sent, total);
                };
                return httpRequest(q, p.host);
            };

            auto res = upload();
            if (saysExists(res)) {
                emit(TransferState::Uploading, "The file exists on the device. Replacing it.", 0, 0, total);
                std::string form = formEncode({{"paths", nlohmann::json::array({remote}).dump()}});
                HttpRequest q;
                q.method = "POST";
                q.url = base + "/delete";
                q.body = &form;
                q.headers.push_back(formType);
                q.cancel = cancel;
                auto del = httpRequest(q, p.host);
                if (del.status < 200 || del.status >= 300) {
                    throw NetError(TransferErrorCode::Exists,
                                   file.name + " already exists on the device, and the device did not delete it (HTTP " +
                                       std::to_string(del.status) + "). Rename the file or delete it on the device.");
                }
                res = upload();
                if (saysExists(res))
                    throw NetError(TransferErrorCode::Exists,
                                   file.name + " already exists on the device. Rename the file or delete it on the device.");
            }
            if (res.status < 200 || res.status >= 300) throw httpError(res.status, "The upload", res.body);

            // 3. Verifying: list the folder and compare the size.
            emit(TransferState::Verifying, "Checking the file on the device", 1, total, total);
            HttpRequest lq;
            lq.url = base + "/api/files?path=" + encodeComponent(dir);
            lq.cancel = cancel;
            auto list = httpRequest(lq, p.host);
            if (list.status < 200 || list.status >= 300) throw httpError(list.status, "The check after upload");
            auto j = nlohmann::json::parse(list.body, nullptr, false);
            if (j.is_discarded()) throw NetError(TransferErrorCode::Device, "The device returned a file list that is not JSON.");
            const nlohmann::json* found = nullptr;
            if (j.is_array()) {
                for (const auto& e : j) {
                    if (!e.is_object()) continue;
                    if (e.value("name", std::string()) == file.name && !e.value("isDirectory", false)) {
                        found = &e;
                        break;
                    }
                }
            }
            if (!found)
                throw NetError(TransferErrorCode::Device,
                               "The upload finished, but " + file.name + " is not in " + dir + " on the device.");
            if (found->contains("size") && (*found)["size"].is_number()) {
                auto size = (*found)["size"].get<uint64_t>();
                if (size != total)
                    throw NetError(TransferErrorCode::Device, "The device reports " + std::to_string(size) +
                                                                  " bytes, but the file has " + std::to_string(total) +
                                                                  " bytes. Send the file again.");
            }
            return remote;
        });
    }
};

Transport& crosspointHttpTransport() {
    static CrossPointHttpTransport t;
    return t;
}

} // namespace ks::net::detail
