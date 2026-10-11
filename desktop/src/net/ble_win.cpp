// Bluetooth LE link for Windows, with C++/WinRT (MSYS2 package
// mingw-w64-x86_64-cppwinrt). This is the only file that includes WinRT
// headers; it compiles as C++20 (see src/net/sources.cmake).
//
// Threading: every call runs on the caller's worker thread. The first call
// on a thread joins the multi-threaded apartment. Async operations are
// awaited with a polling wait_for() so timeouts and cancel work; no
// coroutines. STATUS notifications arrive on a WinRT thread-pool thread.
// No pairing: the characteristics are unencrypted (docs/BLE_PROTOCOL.md).
#include "net/ble_link.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace ks::net::ble {

namespace {

using namespace winrt;
namespace WF = winrt::Windows::Foundation;
namespace WB = winrt::Windows::Devices::Bluetooth;
namespace GATT = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
namespace ADV = winrt::Windows::Devices::Bluetooth::Advertisement;
namespace SS = winrt::Windows::Storage::Streams;
using detail::NetError;
using Clock = std::chrono::steady_clock;

constexpr int kOpTimeoutMs = 10000;

// 6b726472-00XX-4b52-8000-00000000f11e
guid uuidFor(uint16_t xx) {
    return guid(0x6b726472u, xx, 0x4b52, std::array<uint8_t, 8>{0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf1, 0x1e});
}

void ensureApartment() {
    thread_local bool done = false;
    if (done) return;
    try {
        init_apartment(apartment_type::multi_threaded);
    } catch (const hresult_error&) {
        // The thread already has an apartment. Blocking waits still work on MTA.
    }
    done = true;
}

std::string hresultText(const hresult_error& e) {
    std::string m = to_string(e.message());
    while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ' || m.back() == '.')) m.pop_back();
    char code[16];
    std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(e.code().value));
    return m.empty() ? std::string(code) : m + " (" + code + ")";
}

// Waits for an async operation with a timeout and the cancel flag.
// Note: wait_for() attaches a Completed delegate on every call and an
// operation accepts only one (E_ILLEGAL_DELEGATE_ASSIGNMENT on the second),
// so the handler is attached once and a flag is polled instead.
struct AwaitState {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};

template <typename Op>
auto awaitOp(const Op& op, int timeoutMs, const bool* cancel, const std::string& what) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    try {
        auto state = std::make_shared<AwaitState>();
        op.Completed([state](const auto&, WF::AsyncStatus) {
            {
                std::lock_guard<std::mutex> lk(state->m);
                state->done = true;
            }
            state->cv.notify_all();
        });
        std::unique_lock<std::mutex> lk(state->m);
        while (!state->done) {
            state->cv.wait_for(lk, std::chrono::milliseconds(50));
            if (state->done) break;
            if (detail::isCancelled(cancel)) {
                lk.unlock();
                op.Cancel();
                throw detail::cancelled();
            }
            if (Clock::now() >= deadline) {
                lk.unlock();
                op.Cancel();
                throw NetError(TransferErrorCode::Network, "Bluetooth: " + what + " timed out.");
            }
        }
        lk.unlock();
        return op.GetResults();
    } catch (const hresult_error& e) {
        throw NetError(TransferErrorCode::Network, "Bluetooth: " + what + " failed: " + hresultText(e));
    }
}

const char* commText(GATT::GattCommunicationStatus s) {
    switch (s) {
        case GATT::GattCommunicationStatus::Success: return "success";
        case GATT::GattCommunicationStatus::Unreachable: return "the reader is unreachable";
        case GATT::GattCommunicationStatus::ProtocolError: return "protocol error";
        case GATT::GattCommunicationStatus::AccessDenied: return "access denied";
    }
    return "unknown status";
}

std::string bytesOf(const SS::IBuffer& buf) {
    std::string s(buf.Length(), '\0');
    if (!s.empty()) {
        auto r = SS::DataReader::FromBuffer(buf);
        auto* p = reinterpret_cast<uint8_t*>(s.data());
        r.ReadBytes(array_view<uint8_t>(p, p + s.size()));
    }
    return s;
}

SS::IBuffer bufferOf(const std::string& s) {
    SS::DataWriter w;
    auto* p = reinterpret_cast<const uint8_t*>(s.data());
    w.WriteBytes(array_view<const uint8_t>(p, p + s.size()));
    return w.DetachBuffer();
}

class WinLink final : public BleLink {
public:
    ~WinLink() override { disconnect(); }

    void connect(uint64_t address, int timeoutMs, const bool* cancel) override {
        ensureApartment();
        disconnect();
        const std::string addr = formatBleAddress(address);
        dev_ = awaitOp(WB::BluetoothLEDevice::FromBluetoothAddressAsync(address), timeoutMs, cancel,
                       "opening " + addr);
        if (!dev_)
            throw NetError(TransferErrorCode::NotFound, "Bluetooth: the reader " + addr + " was not found. " +
                                                            std::string(kBleReaderHint) + ".");
        connTok_ = dev_.ConnectionStatusChanged([this](const WB::BluetoothLEDevice& d, const WF::IInspectable&) {
            if (d.ConnectionStatus() == WB::BluetoothConnectionStatus::Disconnected) connected_ = false;
        });
        try {
            session_ = awaitOp(GATT::GattSession::FromDeviceIdAsync(dev_.BluetoothDeviceId()), timeoutMs, cancel,
                               "opening the GATT session");
            if (session_) session_.MaintainConnection(true);
        } catch (const NetError& e) {
            if (e.err.code == TransferErrorCode::Cancelled) throw;
            session_ = nullptr;   // optional: only for MaintainConnection and the MTU
        }
        auto sr = awaitOp(dev_.GetGattServicesForUuidAsync(uuidFor(0x0001), WB::BluetoothCacheMode::Uncached),
                          timeoutMs, cancel, "connecting to " + addr);
        if (sr.Status() != GATT::GattCommunicationStatus::Success)
            throw NetError(TransferErrorCode::Network, std::string("Bluetooth: could not connect to the reader (") +
                                                           commText(sr.Status()) + "). " + kBleReaderHint + ".");
        auto services = sr.Services();
        if (services.Size() == 0)
            throw NetError(TransferErrorCode::NotFound,
                           "Bluetooth: this device has no CrossPoint file service. " + std::string(kBleReaderHint) + ".");
        svc_ = services.GetAt(0);
        meta_ = characteristic(0x0002, "META", timeoutMs, cancel);
        chunk_ = characteristic(0x0003, "CHUNK", timeoutMs, cancel);
        control_ = characteristic(0x0004, "CONTROL", timeoutMs, cancel);
        status_ = characteristic(0x0005, "STATUS", timeoutMs, cancel);
        info_ = characteristic(0x0006, "INFO", timeoutMs, cancel);
        connected_ = true;
        try {
            if (session_) mtu_ = session_.MaxPduSize();
        } catch (...) {
            mtu_ = 0;
        }
    }

    void subscribeStatus(StatusFn fn) override {
        bool attach = static_cast<bool>(fn);
        {
            std::lock_guard<std::mutex> lk(handlerMu_);
            handler_ = std::move(fn);
        }
        if (!attach || cccd_) return;
        if (!status_) throw NetError(TransferErrorCode::Network, "Bluetooth: not connected.");
        valueTok_ = status_.ValueChanged([this](const GATT::GattCharacteristic&, const GATT::GattValueChangedEventArgs& a) {
            std::string packet;
            try {
                packet = bytesOf(a.CharacteristicValue());
            } catch (...) {
                return;
            }
            // Hold the lock during the call so a detach waits for it.
            std::lock_guard<std::mutex> lk(handlerMu_);
            if (handler_) handler_(packet);
        });
        auto r = awaitOp(status_.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(
                             GATT::GattClientCharacteristicConfigurationDescriptorValue::Notify),
                         kOpTimeoutMs, nullptr, "subscribing to STATUS");
        if (r.Status() != GATT::GattCommunicationStatus::Success)
            throw NetError(TransferErrorCode::Network,
                           std::string("Bluetooth: subscribing to STATUS failed (") + commText(r.Status()) + ").");
        cccd_ = true;
    }

    std::string readInfo() override {
        need(info_);
        auto r = awaitOp(info_.ReadValueAsync(WB::BluetoothCacheMode::Uncached), kOpTimeoutMs, nullptr, "reading INFO");
        if (r.Status() != GATT::GattCommunicationStatus::Success)
            throw NetError(TransferErrorCode::Network,
                           std::string("Bluetooth: reading INFO failed (") + commText(r.Status()) + ").");
        return bytesOf(r.Value());
    }

    void writeMeta(const std::string& bytes) override { write(meta_, bytes, true, "META"); }
    void writeControl(uint8_t opcode) override { write(control_, std::string(1, static_cast<char>(opcode)), true, "CONTROL"); }
    void writeChunk(const std::string& bytes) override { write(chunk_, bytes, false, "CHUNK"); }

    void disconnect() noexcept override {
        connected_ = false;
        {
            std::lock_guard<std::mutex> lk(handlerMu_);
            handler_ = nullptr;
        }
        try {
            if (status_ && valueTok_.value) status_.ValueChanged(valueTok_);
        } catch (...) {
        }
        try {
            if (dev_ && connTok_.value) dev_.ConnectionStatusChanged(connTok_);
        } catch (...) {
        }
        try {
            if (session_) {
                session_.MaintainConnection(false);
                session_.Close();
            }
        } catch (...) {
        }
        try {
            if (svc_) svc_.Close();
        } catch (...) {
        }
        try {
            if (dev_) dev_.Close();
        } catch (...) {
        }
        valueTok_ = {};
        connTok_ = {};
        meta_ = chunk_ = control_ = status_ = info_ = nullptr;
        svc_ = nullptr;
        session_ = nullptr;
        dev_ = nullptr;
        cccd_ = false;
        mtu_ = 0;
    }

    bool connected() const override { return connected_; }
    int mtu() const override { return mtu_; }

private:
    GATT::GattCharacteristic characteristic(uint16_t xx, const char* name, int timeoutMs, const bool* cancel) {
        auto r = awaitOp(svc_.GetCharacteristicsForUuidAsync(uuidFor(xx), WB::BluetoothCacheMode::Uncached), timeoutMs,
                         cancel, std::string("finding ") + name);
        if (r.Status() != GATT::GattCommunicationStatus::Success || r.Characteristics().Size() == 0)
            throw NetError(TransferErrorCode::Device,
                           std::string("Bluetooth: the reader has no ") + name + " characteristic. Update the reader firmware.");
        return r.Characteristics().GetAt(0);
    }

    void need(const GATT::GattCharacteristic& c) const {
        if (!c || !connected_) throw NetError(TransferErrorCode::Network, "Bluetooth: the connection to the reader was lost.");
    }

    void write(const GATT::GattCharacteristic& c, const std::string& bytes, bool withResponse, const char* name) {
        need(c);
        auto opt = withResponse ? GATT::GattWriteOption::WriteWithResponse : GATT::GattWriteOption::WriteWithoutResponse;
        auto r = awaitOp(c.WriteValueWithResultAsync(bufferOf(bytes), opt), kOpTimeoutMs, nullptr,
                         std::string("writing ") + name);
        if (r.Status() != GATT::GattCommunicationStatus::Success)
            throw NetError(TransferErrorCode::Network,
                           std::string("Bluetooth: writing ") + name + " failed (" + commText(r.Status()) + ").");
    }

    WB::BluetoothLEDevice dev_{nullptr};
    GATT::GattSession session_{nullptr};
    GATT::GattDeviceService svc_{nullptr};
    GATT::GattCharacteristic meta_{nullptr}, chunk_{nullptr}, control_{nullptr}, status_{nullptr}, info_{nullptr};
    event_token valueTok_{}, connTok_{};
    bool cccd_ = false;
    std::atomic<bool> connected_{false};
    int mtu_ = 0;
    std::mutex handlerMu_;
    StatusFn handler_;
};

// Shared between the scan and the watcher callback (which can outlive a
// stack frame by a moment after Stop()).
struct ScanState {
    struct Seen {
        bool service = false;
        std::string name;
        int rssi = -127;
    };
    std::mutex mu;
    std::map<uint64_t, Seen> seen;
};

} // namespace

bool supported() { return true; }

std::unique_ptr<BleLink> makeLink() { return std::make_unique<WinLink>(); }

std::vector<ScanResult> scan(int timeoutMs, const bool* cancel) {
    ensureApartment();
    auto adapter = awaitOp(WB::BluetoothAdapter::GetDefaultAsync(), 5000, cancel, "finding the adapter");
    if (!adapter) throw NetError(TransferErrorCode::Unsupported, "This computer has no Bluetooth adapter.");
    if (!adapter.IsLowEnergySupported())
        throw NetError(TransferErrorCode::Unsupported, "The Bluetooth adapter of this computer does not support Bluetooth LE.");
    try {
        auto radio = awaitOp(adapter.GetRadioAsync(), 5000, cancel, "reading the radio state");
        if (radio && radio.State() != winrt::Windows::Devices::Radios::RadioState::On)
            throw NetError(TransferErrorCode::Unsupported, "Bluetooth is off. Turn it on in the Windows settings.");
    } catch (const NetError& e) {
        if (e.err.code != TransferErrorCode::Network) throw;   // radio state unknown: try the scan anyway
    }

    auto state = std::make_shared<ScanState>();
    const guid service = uuidFor(0x0001);
    ADV::BluetoothLEAdvertisementWatcher w;
    w.ScanningMode(ADV::BluetoothLEScanningMode::Active);   // the name is in the scan response
    // No service filter on the watcher: the scan response has the name but
    // not the UUID, and a filter would drop it. Merge by address instead.
    auto tok = w.Received([state, service](const ADV::BluetoothLEAdvertisementWatcher&,
                                           const ADV::BluetoothLEAdvertisementReceivedEventArgs& a) {
        try {
            auto adv = a.Advertisement();
            std::string name = to_string(adv.LocalName());
            bool hasService = false;
            for (const auto& u : adv.ServiceUuids())
                if (u == service) hasService = true;
            std::lock_guard<std::mutex> lk(state->mu);
            auto& s = state->seen[a.BluetoothAddress()];
            if (hasService) s.service = true;
            if (!name.empty()) s.name = name;
            s.rssi = a.RawSignalStrengthInDBm();
        } catch (...) {
        }
    });
    w.Start();
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    bool aborted = false;
    while (Clock::now() < deadline) {
        if (detail::isCancelled(cancel)) break;
        if (w.Status() == ADV::BluetoothLEAdvertisementWatcherStatus::Aborted) {
            aborted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    try {
        w.Stop();
        w.Received(tok);
    } catch (...) {
    }
    if (detail::isCancelled(cancel)) throw detail::cancelled();
    if (aborted)
        throw NetError(TransferErrorCode::Unsupported, "Bluetooth scanning stopped. Check that Bluetooth is on and not in use.");

    std::vector<ScanResult> out;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        for (auto& [addr, s] : state->seen) {
            if (!s.service) continue;
            ScanResult r;
            r.address = addr;
            r.name = s.name.empty() ? fallbackName(addr) : s.name;
            r.rssi = s.rssi;
            out.push_back(r);
        }
    }
    std::sort(out.begin(), out.end(), [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    return out;
}

} // namespace ks::net::ble
