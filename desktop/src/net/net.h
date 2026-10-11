// k-rosspointSender desktop: network, devices and storage (namespace ks::net).
//
// This module contains:
// - The transports (WebDAV, CrossPoint HTTP, CrossPoint WebSocket, Folder,
//   BLE mock). Get one with transport(method).
// - UDP discovery of CrossPoint devices.
// - explain(): plain-word reasons and fallbacks for a failed transfer.
// - Remote folders: list and create folders on the device (folder browser).
// - Device profiles, app settings, the article library and the spider
//   packs. These are JSON files in ks::dataDir().
// - HelperServer: the "local helper app" for the web version, on 127.0.0.1.
//
// All functions are safe to call from a background thread. The storage
// functions use one internal mutex. Do not call them from many processes.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ks::net {

// ------------------------------------------------------------- transports

// The transport for a method. The same object for each call (singleton).
Transport& transport(TransferMethod m);

// All methods, in the order the UI shows them.
std::vector<TransferMethod> methods();

// The WebSocket upload port. CrossPoint uses 81. Tests set another port.
int wsPort();
void setWsPort(int port);

// Note for TransferMethod::Folder: DeviceProfile.host holds the local folder
// (for example "E:/", the SD card in USB Drive mode). The file goes to
// <host>/<targetFolder>/<name>.

// Text form of an error code, the same strings as the web app and the
// helper JSON: "network", "not-found", "auth", "exists", "unsupported",
// "cancelled", "device", "unknown".
std::string toString(TransferErrorCode c);
std::optional<TransferErrorCode> errorCodeFrom(const std::string& s);

// Folder path helpers (same rules as the web app).
std::vector<std::string> folderSegments(const std::string& folder);   // "/Books//Inbox/" -> {"Books","Inbox"}
std::string normalizeFolder(const std::string& folder);               // "/Books/Inbox/", "/" for root
std::string folderNoTrailingSlash(const std::string& folder);         // "/Books/Inbox", "/" for root
std::string remoteFilePath(const std::string& folder, const std::string& name);

// CRC32 (IEEE 802.3, poly 0xEDB88320). The future BLE protocol uses it.
uint32_t crc32(const std::string& bytes);

// ------------------------------------------------------------- remote folders (remote_folder.cpp)

struct RemoteEntry {
    std::string name;
    uint64_t size = 0;
    bool isDirectory = false;
};

// True for the methods that can list folders: CrossPoint HTTP and WebDAV.
bool canBrowseRemote(TransferMethod m);

// Lists one folder on the device. Folders first, then files, each sorted
// by name. CrossPoint HTTP: GET /api/files?path=<dir>. WebDAV: PROPFIND
// with Depth 1. Other methods throw TransferError (Unsupported). A network
// or HTTP failure also throws TransferError.
std::vector<RemoteEntry> listRemoteFolder(const DeviceProfile& p, const std::string& path);

// Makes the folder <path>/<name> on the device. CrossPoint HTTP: POST
// /mkdir (form name, path). WebDAV: MKCOL; 405 (the folder exists) is not
// an error. Throws TransferError on failure or for a bad name.
void makeRemoteFolder(const DeviceProfile& p, const std::string& path, const std::string& name);

// Parses a WebDAV multistatus body. Any namespace prefix is accepted. The
// entry for requestPath itself is not in the result.
std::vector<RemoteEntry> parsePropfind(const std::string& xml, const std::string& requestPath);

// ------------------------------------------------------------- discovery

struct Discovered {
    std::string ip;
    std::string hostname;
    int wsPort = 81;
    std::string raw;
};

// Sends "hello" to UDP port 8134 on 255.255.255.255 and on the broadcast
// address of each IPv4 interface. Collects the replies until the timeout.
// One result per IP. When no device replies, tries the mDNS name
// crosspoint.local as a fallback.
std::vector<Discovered> discoverCrossPoint(int timeoutMs = 1500);

// Parses one reply "crosspoint (on <host>);<port>". Sets hostname, wsPort
// and raw. Does not set ip. Returns false when the text is not a reply.
bool parseDiscoveryReply(const std::string& raw, Discovered& out);

// ------------------------------------------------------------- explain

struct Explanation {
    std::string title;
    std::vector<std::string> reasons;
    std::vector<std::string> fallbacks;
};
Explanation explain(const TransferError& e);

// ------------------------------------------------------------- profiles (devices.json)

std::vector<DeviceProfile> listProfiles();                 // oldest first
std::optional<DeviceProfile> getProfile(const std::string& id);
DeviceProfile saveProfile(DeviceProfile p);                // fills id/timestamps; throws std::runtime_error with the problem list
void deleteProfile(const std::string& id);
void markConnected(const std::string& id);
DeviceProfile defaultProfile(FirmwareType f);
std::vector<std::string> validateProfile(const DeviceProfile& p);   // empty = valid
bool isValidHost(const std::string& host);                 // host name (RFC 1123 labels) or IPv4

// ------------------------------------------------------------- settings (settings.json)

AppSettings loadSettings();                                // stored values merge over the defaults
void saveSettings(const AppSettings& s);

// ------------------------------------------------------------- library (library/<id>/entry.json + images/)

void saveEntry(const LibraryEntry& e, const ImageMap* images = nullptr);   // images == nullptr keeps the stored images
std::optional<LibraryEntry> getEntry(const std::string& id);
std::vector<LibraryEntry> listEntries();                   // newest first (updatedAt)
ImageMap loadImages(const std::string& id);
void deleteEntry(const std::string& id);
void clearLibrary();                                       // also removes every pack
void addTransfer(const std::string& articleId, const TransferRecord& r);   // throws std::runtime_error when the entry does not exist

// ------------------------------------------------------------- packs (packs/<id>/pack.json + images/, packs.cpp)

// pack.json holds every field of Pack, the pages with their articles too.
// images == nullptr keeps the stored images. Throws std::runtime_error when
// the pack id is empty or not safe as a folder name.
void savePack(const Pack& pack, const ImageMap* images = nullptr);
std::optional<Pack> getPack(const std::string& id);
std::vector<Pack> listPacks();                             // newest first (updatedAt)
ImageMap loadPackImages(const std::string& id);
void deletePack(const std::string& id);

// Library data of a pack that is not in Pack: the last file name and the
// sends. savePack keeps these fields.
struct PackRecord {
    std::string fileName;
    std::vector<TransferRecord> transfers;
};
PackRecord getPackRecord(const std::string& id);
// Sets the file name (when not empty) and adds the transfer (when set).
// Does nothing when the pack does not exist.
void updatePackRecord(const std::string& id, const std::string& fileName, const std::optional<TransferRecord>& rec);

// ------------------------------------------------------------- helper server

// The "local helper app" for the web version. Same API as helper/helper.py:
//   GET  /status    {"ok":true,"version":"desktop-0.1"}
//   POST /send      multipart file, host, port, folder, method (webdav | crosspoint-http),
//                   optional username, password
//   POST /test      JSON {host, port}, proxies the device /api/status
//   GET  /discover  JSON list from discoverCrossPoint()
//   OPTIONS *       204 with CORS headers
// Listens on 127.0.0.1 only, on its own thread.
class HelperServer {
public:
    HelperServer();
    ~HelperServer();
    HelperServer(const HelperServer&) = delete;
    HelperServer& operator=(const HelperServer&) = delete;

    // Starts the listener. port 0 picks a free port (see port()).
    // Returns false and sets lastError() when the port is not free.
    bool start(int port, const std::vector<std::string>& allowedOrigins);
    void stop();
    bool running() const;
    std::string lastError() const;
    int port() const;   // the bound port, 0 when not running

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// True when a browser origin may call the helper. Exact match with an
// allowed entry, or, for an entry "http://localhost" or "http://127.0.0.1",
// the same origin with any ":<port>". "null" and empty are never allowed.
bool originAllowed(const std::string& origin, const std::vector<std::string>& allowed);

} // namespace ks::net
