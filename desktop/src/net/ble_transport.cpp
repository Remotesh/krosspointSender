// Bluetooth LE transport (TransferMethod::Ble): CrossPoint "Receive via
// Bluetooth". Platform independent. The link comes from ble::makeLink()
// (ble_win.cpp on Windows, ble_stub.cpp elsewhere); the protocol is
// ble::Session (ble_protocol.cpp). The contract is docs/BLE_PROTOCOL.md.
#include "net/ble_protocol.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace ks::net {

using detail::NetError;

// ------------------------------------------------------------- address helpers

std::string formatBleAddress(uint64_t a) {
    if (!a) return {};
    char buf[24];
    std::snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", static_cast<unsigned>((a >> 40) & 0xFF),
                  static_cast<unsigned>((a >> 32) & 0xFF), static_cast<unsigned>((a >> 24) & 0xFF),
                  static_cast<unsigned>((a >> 16) & 0xFF), static_cast<unsigned>((a >> 8) & 0xFF),
                  static_cast<unsigned>(a & 0xFF));
    return buf;
}

std::optional<uint64_t> parseBleAddress(const std::string& s) {
    std::string hex;
    for (char c : s) {
        if (c == ':' || c == '-' || c == ' ') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
        hex += c;
    }
    if (hex.size() != 12) return std::nullopt;
    uint64_t v = std::stoull(hex, nullptr, 16);
    if (!v) return std::nullopt;
    return v;
}

namespace ble {

std::string fallbackName(uint64_t address) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "CrossPoint-%04X", static_cast<unsigned>(address & 0xFFFF));
    return buf;
}

} // namespace ble

bool bleSupported() { return ble::supported(); }

std::vector<BleDiscovered> discoverBle(int timeoutMs, const bool* cancel) {
    try {
        return ble::scan(timeoutMs, cancel);
    } catch (const NetError& e) {
        throw e.err;
    }
}

// ------------------------------------------------------------- transport

namespace detail {

namespace {

constexpr int kConnectTimeoutMs = 15000;
constexpr int kScanMs = 4000;

std::string where(const DeviceProfile& p) {
    std::string n = p.bleName.empty() ? std::string("the reader") : p.bleName;
    if (p.bleAddress) n += " (" + formatBleAddress(p.bleAddress) + ")";
    return n;
}

// The address to connect to: the profile address, or a scan for the name.
uint64_t resolveAddress(const DeviceProfile& p, const Emitter& emit, const bool* cancel) {
    if (p.bleAddress) return p.bleAddress;
    if (p.bleName.empty())
        throw NetError(TransferErrorCode::NotFound,
                       "No Bluetooth reader is chosen for this device. Use Bluetooth discover in the devices window.");
    emit(TransferState::Connecting, "Looking for " + p.bleName + " over Bluetooth");
    for (auto& d : ble::scan(kScanMs, cancel))
        if (toLower(d.name) == toLower(p.bleName)) return d.address;
    throw NetError(TransferErrorCode::NotFound, "The reader " + p.bleName + " was not found over Bluetooth. " +
                                                    std::string(kBleReaderHint) + ".");
}

// Disconnects the link when the scope ends.
struct LinkGuard {
    ble::BleLink& link;
    ~LinkGuard() { link.disconnect(); }
};

} // namespace

class BleTransport final : public Transport {
public:
    TransferMethod method() const override { return TransferMethod::Ble; }
    bool isSupported() const override { return ble::supported(); }
    std::vector<std::string> limitations() const override {
        return {std::string(kBleReaderHint) + ". The reader must be a CrossPoint build with Bluetooth transfer (ESP32-S3, for example the X4 Pro).",
                "Bluetooth sends 10 to 50 KB/s. Files over 3 MB are slow; files over 16 MB are refused.",
                "No pairing is needed. Keep the reader near this computer."};
    }

    ConnectionTestResult testConnection(const DeviceProfile& p) override {
        ConnectionTestResult res;
        auto t0 = std::chrono::steady_clock::now();
        try {
            if (!ble::supported()) throw NetError(TransferErrorCode::Unsupported, "This build has no Bluetooth LE support (Windows only).");
            ProgressFn none;
            Emitter emit(none);
            uint64_t addr = resolveAddress(p, emit, nullptr);
            auto link = ble::makeLink();
            LinkGuard guard{*link};
            link->connect(addr, kConnectTimeoutMs, nullptr);
            ble::Session s(*link);
            ble::Info info = s.ping(nullptr);
            res.ok = true;
            res.latencyMs = msSince(t0);
            nlohmann::json j = {{"address", formatBleAddress(addr)},
                                {"name", p.bleName},
                                {"protocolVersion", info.protocolVersion},
                                {"maxChunkData", info.maxChunkData},
                                {"freeKiB", info.freeKiB},
                                {"firmware", info.firmware},
                                {"mtu", link->mtu()}};
            res.info = j.dump();
        } catch (const NetError& e) {
            res.ok = false;
            res.error = e.err;
        } catch (const std::exception& e) {
            res.ok = false;
            res.error = makeError(TransferErrorCode::Unknown, e.what());
        }
        return res;
    }

    TransferResult send(const DeviceProfile& p, const OutputFile& file, const ProgressFn& onProgress,
                        const bool* cancel) override {
        return runSend(file, onProgress, [&]() -> std::string {
            Emitter emit(onProgress);
            // Size rules first: refuse before connecting.
            if (auto e = ble::sizeProblem(file.bytes.size())) throw NetError(*e);
            if (!ble::supported()) throw NetError(TransferErrorCode::Unsupported, "This build has no Bluetooth LE support (Windows only).");
            if (std::string w = ble::sizeWarning(file.bytes.size()); !w.empty()) emit(TransferState::Connecting, w);
            if (isCancelled(cancel)) throw cancelled();
            uint64_t addr = resolveAddress(p, emit, cancel);
            DeviceProfile q = p;
            q.bleAddress = addr;
            emit(TransferState::Connecting, "Connecting to " + where(q) + " over Bluetooth");
            auto link = ble::makeLink();
            LinkGuard guard{*link};
            link->connect(addr, kConnectTimeoutMs, cancel);
            if (int mtu = link->mtu(); mtu > 0)
                emit(TransferState::Connecting, "Connected, ATT MTU " + std::to_string(mtu));
            ble::Session s(*link);
            std::string folder = p.targetFolder.empty() ? "/Books/K-rosspoint/" : p.targetFolder;
            return s.sendFile(file, folder, onProgress, cancel);
        });
    }
};

Transport& bleTransport() {
    static BleTransport t;
    return t;
}

} // namespace detail

} // namespace ks::net
