// Bluetooth LE link for platforms without an implementation (not Windows).
// Every call reports Unsupported.
#include "net/ble_link.h"

namespace ks::net::ble {

namespace {
detail::NetError unsupportedError() {
    return detail::NetError(TransferErrorCode::Unsupported,
                            "Bluetooth transfer is available only in the Windows build. Use Wi-Fi or the USB folder method.");
}
} // namespace

bool supported() { return false; }

std::unique_ptr<BleLink> makeLink() { throw unsupportedError(); }

std::vector<ScanResult> scan(int, const bool*) { throw unsupportedError(); }

} // namespace ks::net::ble
