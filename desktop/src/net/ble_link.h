// BLE link: the GATT client side of the CrossPoint BLE file transfer
// (docs/BLE_PROTOCOL.md at the repository root). Internal to src/net.
//
// BleLink is a plain C++ interface. The Windows build implements it with
// C++/WinRT in ble_win.cpp; other platforms get ble_stub.cpp (unsupported).
// The protocol driver (ble_protocol.h) runs on top of it, and the unit test
// drives it with a fake link. No WinRT type appears in this header.
//
// Every method throws detail::NetError on failure. Call all methods from one
// worker thread; only the STATUS callback runs on another thread.
#pragma once

#include "net/net_internal.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ks::net::ble {

// UUIDs. Base 6b726472-00XX-4b52-8000-00000000f11e ("krdr").
constexpr const char* kServiceUuid = "6b726472-0001-4b52-8000-00000000f11e";
constexpr const char* kMetaUuid = "6b726472-0002-4b52-8000-00000000f11e";
constexpr const char* kChunkUuid = "6b726472-0003-4b52-8000-00000000f11e";
constexpr const char* kControlUuid = "6b726472-0004-4b52-8000-00000000f11e";
constexpr const char* kStatusUuid = "6b726472-0005-4b52-8000-00000000f11e";
constexpr const char* kInfoUuid = "6b726472-0006-4b52-8000-00000000f11e";

class BleLink {
public:
    // Receives one STATUS notification (raw bytes). Runs on a stack thread.
    using StatusFn = std::function<void(const std::string& packet)>;

    virtual ~BleLink() = default;

    // Connects to the reader and finds FILE_SERVICE and its characteristics.
    // No pairing. Polls *cancel while it waits.
    virtual void connect(uint64_t address, int timeoutMs, const bool* cancel) = 0;
    // Sets the STATUS handler and enables notifications (the CCCD write
    // happens once per link). An empty fn only detaches the handler.
    virtual void subscribeStatus(StatusFn fn) = 0;
    // Reads INFO.
    virtual std::string readInfo() = 0;
    // Writes META / CONTROL with response.
    virtual void writeMeta(const std::string& bytes) = 0;
    virtual void writeControl(uint8_t opcode) = 0;
    // Writes one CHUNK without response.
    virtual void writeChunk(const std::string& bytes) = 0;
    // Closes the link. Never throws. Safe to call more than once.
    virtual void disconnect() noexcept = 0;
    // False after the link drops or before connect().
    virtual bool connected() const = 0;
    // Negotiated ATT MTU (GattSession.MaxPduSize) when known, else 0. Logging only.
    virtual int mtu() const { return 0; }
};

// One reader found by a scan (net::BleDiscovered in net.h).
using ScanResult = BleDiscovered;

// True when this build has a BLE implementation (Windows).
bool supported();
// A new, unconnected link. Throws NetError (Unsupported) on other platforms.
std::unique_ptr<BleLink> makeLink();
// Scans advertisements for timeoutMs and returns the devices that advertise
// FILE_SERVICE, strongest first. Throws NetError (Unsupported) when there is
// no Bluetooth adapter or it is off. Throws cancelled() on cancel.
std::vector<ScanResult> scan(int timeoutMs, const bool* cancel);

// "CrossPoint-3A7F" from the last two address bytes.
std::string fallbackName(uint64_t address);

} // namespace ks::net::ble
