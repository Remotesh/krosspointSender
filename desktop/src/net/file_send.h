// "Send a file": push any file from disk to the reader's SD card over the
// normal transports (for example firmware.bin to "/" for the reader's
// Settings > System > SD Card Firmware Update screen). No conversion and no
// library article. The UI (app/send_file.cpp) and the tests use these
// functions; the CLI --send path follows the same rules.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ks::net {

// Files over this size are refused for every method.
constexpr uint64_t kMaxSendFileBytes = 256ull * 1024 * 1024;

// The hint after a successful .bin send.
constexpr const char* kFirmwareHint =
    "On the reader: Settings > System > SD Card Firmware Update, then pick this file.";
// The note for Bluetooth and firmware.
constexpr const char* kBleFirmwareNote =
    "Wi-Fi (CrossPoint or WebDAV) is better for firmware: Bluetooth is slow and allows at most 16 MB.";

// True for files that the app converts as an article: html, htm, xhtml,
// md, markdown, txt, epub, url. Other files are offered as "send as file".
bool isArticleInputPath(const std::string& path);
// True when the name ends in .bin (case-insensitive).
bool isFirmwareName(const std::string& name);
// The last path part ("C:\a\b.bin" -> "b.bin").
std::string fileBaseName(const std::string& path);
// MIME type from the extension. Unknown types: application/octet-stream.
std::string mimeForName(const std::string& name);

// The OutputFile for raw bytes: name as given, mime from the extension,
// format from the extension (Epub when the extension is not an output format).
OutputFile makeRawFile(const std::string& name, std::string bytes);

// A problem with the file name, or "" when it is good. Rejects an empty
// name, "/" or "\", "." or "..", control characters, more than 255 bytes
// (Bluetooth: 200 bytes).
std::string sendNameProblem(const std::string& name, TransferMethod m);

// The checks before a send. problems block the send; warnings do not.
struct FileSendCheck {
    std::vector<std::string> problems;
    std::vector<std::string> warnings;
    bool ok() const { return problems.empty(); }
};
// Size rules: 0 bytes and > 256 MB are refused. Bluetooth: > 16 MB refused,
// > 3 MB warned, and a .bin gets the Wi-Fi note.
FileSendCheck checkFileSend(TransferMethod m, const std::string& name, uint64_t bytes);

// The profile with the destination folder set (normalized; "" = the
// profile folder).
DeviceProfile withFolder(const DeviceProfile& p, const std::string& folder);

// Checks, then sends through t with the destination folder. On a check
// problem nothing is sent and the result has an Unsupported error.
TransferResult sendRawFile(Transport& t, const DeviceProfile& p, const std::string& folder, const OutputFile& f,
                           const ProgressFn& onProgress, const bool* cancel = nullptr);

// ------------------------------------------------------------- history (file_sends.json)

// The record of a file send: articleId is empty, fileName is the remote
// path when the send worked (else the file name).
TransferRecord makeFileSendRecord(const DeviceProfile& p, const OutputFile& f, const TransferResult& res);
// Adds a record (newest first, at most 200 kept).
void addFileSend(const TransferRecord& r);
std::vector<TransferRecord> listFileSends();   // newest first
void clearFileSends();

} // namespace ks::net
