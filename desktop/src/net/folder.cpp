// Folder transport (USB Drive mode). Put the X4 Pro in USB Drive mode. The
// SD card shows as a drive. DeviceProfile.host holds the local folder (the
// drive root, for example "E:/"). The file goes to <host>/<targetFolder>/<name>.
// We write <name>.part in 64 KiB steps with progress, rename it, then
// compare the size on disk.
#include "net/net_internal.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace ks::net::detail {

namespace {

constexpr size_t kStep = 64 * 1024;

NetError noFolder(const std::string& root) {
    if (root.empty())
        return NetError(TransferErrorCode::NotFound,
                        "No folder is set for this device. Enter the drive root (for example E:/) in the host field.");
    return NetError(TransferErrorCode::NotFound,
                    "The folder " + root + " does not exist. Connect the reader in USB Drive mode and check the drive letter.");
}

} // namespace

class FolderTransport final : public Transport {
public:
    TransferMethod method() const override { return TransferMethod::Folder; }
    bool isSupported() const override { return true; }
    std::vector<std::string> limitations() const override {
        return {"Put the reader in USB Drive mode and connect it with a cable.",
                "Set the drive root (for example E:/) as the folder of this device.",
                "Eject the drive before you unplug the cable."};
    }

    ConnectionTestResult testConnection(const DeviceProfile& p) override {
        ConnectionTestResult res;
        auto t0 = std::chrono::steady_clock::now();
        std::error_code ec;
        fs::path root = fs::u8path(p.host);
        if (p.host.empty() || !fs::is_directory(root, ec)) {
            res.error = noFolder(p.host).err;
        } else {
            nlohmann::json info = {{"folder", p.host}, {"targetFolder", normalizeFolder(p.targetFolder)}};
            auto sp = fs::space(root, ec);
            if (!ec) info["freeBytes"] = static_cast<uint64_t>(sp.available);
            res.ok = true;
            res.info = info.dump();
        }
        res.latencyMs = msSince(t0);
        return res;
    }

    TransferResult send(const DeviceProfile& p, const OutputFile& file, const ProgressFn& onProgress,
                        const bool* cancel) override {
        return runSend(file, onProgress, [&]() -> std::string {
            Emitter emit(onProgress);
            const uint64_t total = file.bytes.size();
            emit(TransferState::Connecting, "Opening the folder " + p.host);
            std::error_code ec;
            fs::path root = fs::u8path(p.host);
            if (p.host.empty() || !fs::is_directory(root, ec)) throw noFolder(p.host);
            if (file.name.empty() || file.name.find_first_of("/\\") != std::string::npos)
                throw NetError(TransferErrorCode::Device, "The file name is empty or contains a slash.");

            fs::path dir = root;
            for (const auto& seg : folderSegments(p.targetFolder)) dir /= fs::u8path(seg);
            fs::create_directories(dir, ec);
            if (!fs::is_directory(dir))
                throw NetError(TransferErrorCode::Device, "Could not create the folder " + dir.u8string() +
                                                              ". The drive can be read-only or full.");
            fs::path target = dir / fs::u8path(file.name);
            if (fs::is_directory(target, ec))
                throw NetError(TransferErrorCode::Device, "A folder has the same name as the file in the target path.");
            fs::path part = target;
            part += ".part";

            emit(TransferState::Uploading, "Writing " + file.name, 0, 0, total);
            {
                std::ofstream out(part, std::ios::binary | std::ios::trunc);
                if (!out) throw NetError(TransferErrorCode::Device, "Could not write to " + dir.u8string() + ". The drive can be read-only.");
                size_t off = 0;
                while (off < file.bytes.size()) {
                    if (isCancelled(cancel)) {
                        out.close();
                        fs::remove(part, ec);
                        throw cancelled();
                    }
                    size_t n = std::min(kStep, file.bytes.size() - off);
                    out.write(file.bytes.data() + off, static_cast<std::streamsize>(n));
                    if (!out) {
                        out.close();
                        fs::remove(part, ec);
                        throw NetError(TransferErrorCode::Device, "The write failed. The drive can be full.");
                    }
                    off += n;
                    emit(TransferState::Uploading, {}, total ? static_cast<double>(off) / static_cast<double>(total) : 1, off, total);
                }
                out.flush();
                if (!out) {
                    out.close();
                    fs::remove(part, ec);
                    throw NetError(TransferErrorCode::Device, "The write failed. The drive can be full.");
                }
            }
            fs::remove(target, ec);
            ec.clear();
            fs::rename(part, target, ec);
            if (ec) {
                fs::remove(part, ec);
                throw NetError(TransferErrorCode::Device, "Could not rename the file on the drive.");
            }

            emit(TransferState::Verifying, "Checking the file size", 1, total, total);
            auto size = fs::file_size(target, ec);
            if (ec || size != total)
                throw NetError(TransferErrorCode::Device, "The drive reports " + std::to_string(ec ? 0 : size) +
                                                              " bytes, but the file has " + std::to_string(total) + " bytes.");
            return remoteFilePath(p.targetFolder, file.name);
        });
    }
};

Transport& folderTransport() {
    static FolderTransport t;
    return t;
}

} // namespace ks::net::detail
