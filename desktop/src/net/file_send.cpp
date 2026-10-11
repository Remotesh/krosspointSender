// "Send a file" rules and history. See file_send.h.
#include "net/file_send.h"

#include "net/ble_protocol.h"
#include "net/store_json.h"

#include <cstdio>

namespace ks::net {

namespace {

std::string extLower(const std::string& name) {
    std::string base = fileBaseName(name);
    size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? std::string() : toLower(base.substr(dot + 1));
}

std::string mbText(uint64_t n) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(n) / (1024.0 * 1024.0));
    return buf;
}

std::string historyPath() { return joinPath(dataDir(), "file_sends.json"); }

constexpr size_t kHistoryMax = 200;

} // namespace

bool isArticleInputPath(const std::string& path) {
    std::string e = extLower(path);
    return e == "html" || e == "htm" || e == "xhtml" || e == "md" || e == "markdown" || e == "txt" || e == "epub" ||
           e == "url";
}

bool isFirmwareName(const std::string& name) { return extLower(name) == "bin"; }

std::string fileBaseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string mimeForName(const std::string& name) {
    std::string e = extLower(name);
    if (e == "epub") return "application/epub+zip";
    if (e == "html" || e == "htm") return "text/html";
    if (e == "xhtml") return "application/xhtml+xml";
    if (e == "txt") return "text/plain";
    if (e == "md" || e == "markdown") return "text/markdown";
    if (e == "zip") return "application/zip";
    if (e == "pdf") return "application/pdf";
    if (e == "png") return "image/png";
    if (e == "jpg" || e == "jpeg") return "image/jpeg";
    if (e == "bmp") return "image/bmp";
    if (e == "json") return "application/json";
    return "application/octet-stream";
}

OutputFile makeRawFile(const std::string& name, std::string bytes) {
    OutputFile f;
    f.name = name;
    f.mime = mimeForName(name);
    f.format = outputFormatFrom(extLower(name)).value_or(OutputFormat::Epub);
    f.bytes = std::move(bytes);
    return f;
}

std::string sendNameProblem(const std::string& name, TransferMethod m) {
    if (trim(name).empty()) return "The file name is empty.";
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return "The file name must not contain / or \\.";
    if (name == "." || name == "..") return "The file name must not be . or ..";
    for (unsigned char c : name)
        if (c < 0x20 || c == 0x7f) return "The file name must not contain control characters.";
    size_t max = m == TransferMethod::Ble ? ble::kMaxNameBytes : 255;
    if (name.size() > max) return "The file name is longer than " + std::to_string(max) + " bytes.";
    return {};
}

FileSendCheck checkFileSend(TransferMethod m, const std::string& name, uint64_t bytes) {
    FileSendCheck c;
    std::string np = sendNameProblem(name, m);
    if (!np.empty()) c.problems.push_back(np);
    if (bytes == 0) c.problems.push_back("The file is empty.");
    else if (bytes > kMaxSendFileBytes)
        c.problems.push_back("The file is " + mbText(bytes) + ". The app sends files of at most 256 MB.");
    if (m == TransferMethod::Ble) {
        if (bytes > 0 && bytes <= kMaxSendFileBytes) {
            if (auto e = ble::sizeProblem(bytes)) c.problems.push_back(e->message);
        }
        std::string w = ble::sizeWarning(bytes);
        if (!w.empty() && bytes <= ble::kMaxFileBytes) c.warnings.push_back(w);
        if (isFirmwareName(name)) c.warnings.push_back(kBleFirmwareNote);
    }
    return c;
}

DeviceProfile withFolder(const DeviceProfile& p, const std::string& folder) {
    DeviceProfile q = p;
    std::string f = trim(folder);
    q.targetFolder = normalizeFolder(f.empty() ? p.targetFolder : f);
    return q;
}

TransferResult sendRawFile(Transport& t, const DeviceProfile& p, const std::string& folder, const OutputFile& f,
                           const ProgressFn& onProgress, const bool* cancel) {
    FileSendCheck c = checkFileSend(p.method, f.name, f.bytes.size());
    if (!c.ok()) {
        TransferResult r;
        r.ok = false;
        r.startedAt = r.finishedAt = nowIso();
        std::string msg;
        for (auto& s : c.problems) msg += (msg.empty() ? "" : " ") + s;
        r.error = TransferError{TransferErrorCode::Unsupported, msg};
        return r;
    }
    return t.send(withFolder(p, folder), f, onProgress, cancel);
}

TransferRecord makeFileSendRecord(const DeviceProfile& p, const OutputFile& f, const TransferResult& res) {
    TransferRecord r;
    r.id = newUuid();
    r.deviceId = p.id;
    r.deviceName = p.name;
    r.method = p.method;
    r.format = f.format;
    r.fileName = res.ok && !res.remotePath.empty() ? res.remotePath : f.name;
    r.bytes = res.ok ? res.bytes : f.bytes.size();
    r.ok = res.ok;
    if (res.error) r.error = res.error->message;
    r.at = nowIso();
    return r;
}

void addFileSend(const TransferRecord& r) {
    std::lock_guard<std::recursive_mutex> lock(detail::storeMutex());
    detail::json j = detail::readJson(historyPath());
    detail::json list = detail::json::array();
    detail::json item = detail::transferToJson(r);
    item["kind"] = "file";
    list.push_back(item);
    if (j.is_array())
        for (auto& e : j) {
            if (list.size() >= kHistoryMax) break;
            list.push_back(e);
        }
    detail::writeJson(historyPath(), list);
}

std::vector<TransferRecord> listFileSends() {
    std::lock_guard<std::recursive_mutex> lock(detail::storeMutex());
    detail::json j = detail::readJson(historyPath());
    std::vector<TransferRecord> out;
    if (j.is_array())
        for (auto& e : j) out.push_back(detail::transferFromJson(e));
    return out;
}

void clearFileSends() {
    std::lock_guard<std::recursive_mutex> lock(detail::storeMutex());
    detail::writeJson(historyPath(), detail::json::array());
}

} // namespace ks::net
