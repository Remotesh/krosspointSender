// Plain-word explanation of a failed transfer, with the fallback list.
// The desktop app has no browser limits (no mixed content, no CORS), so the
// reasons are about the device, the network and the drive.
#include "net/net_internal.h"

namespace ks::net {

namespace {

const char* kFolder =
    "Write to a folder: put the reader in USB Drive mode, connect the cable, and use the Folder method with the drive root (for example E:/).";
const char* kDownload =
    "Export the file and copy it to the reader yourself, or upload it on the CrossPoint web page.";
const char* kScreen =
    "Open the File Transfer screen on the reader. The device answers only while this screen is open.";
const char* kNetwork =
    "Connect this computer and the reader to the same Wi-Fi network. Click Discover to find the reader IP address.";
const char* kHotspot =
    "Use the reader hotspot: join the Wi-Fi network \"CrossPoint-Reader\" and send to 192.168.4.1.";
const char* kWebDav = "Use the WebDAV method. It replaces a file with the same name.";

} // namespace

Explanation explain(const TransferError& e) {
    Explanation x;
    const std::vector<std::string> lan = {kScreen, kNetwork, kHotspot, kFolder, kDownload};
    switch (e.code) {
        case TransferErrorCode::Network:
            x.title = "Could not reach the device";
            x.reasons = {
                "The device did not answer, or the connection broke.",
                "Check that the reader shows the File Transfer screen and is on the same Wi-Fi as this computer.",
                "Check the host name or IP address. If crosspoint.local does not work, use the IP address shown on the reader.",
                "A firewall on this computer can block the connection.",
            };
            if (!e.message.empty()) x.reasons.insert(x.reasons.begin(), e.message);
            x.fallbacks = lan;
            break;
        case TransferErrorCode::NotFound:
            x.title = "The device or the file location was not found";
            x.reasons = {
                "The device did not answer in time, or it returned \"not found\" (404).",
                "Check the address and the port (80 for CrossPoint). Wake the reader and open the File Transfer screen.",
                "Check the target folder. It must start with \"/\".",
            };
            if (!e.message.empty()) x.reasons.insert(x.reasons.begin(), e.message);
            x.fallbacks = lan;
            break;
        case TransferErrorCode::Auth:
            x.title = "The device refused access";
            x.reasons = {
                "The device asked for a username and password (401/403), or the folder is not writable.",
                "Check the username and password in the device profile. CrossPoint needs none.",
            };
            x.fallbacks = {kFolder, kDownload};
            break;
        case TransferErrorCode::Exists:
            x.title = "The file already exists on the device";
            x.reasons = {
                "CrossPoint does not replace a file with the same name through this upload.",
                "Change the file name, delete the old file on the reader, or use WebDAV (it replaces the file).",
            };
            x.fallbacks = {kWebDav, kFolder, kDownload};
            break;
        case TransferErrorCode::Unsupported:
            x.title = "This transfer method is not available";
            x.reasons = {e.message.empty() ? "This build cannot do this transfer." : e.message};
            x.fallbacks = {kWebDav, kFolder, kDownload};
            break;
        case TransferErrorCode::Cancelled:
            x.title = "The transfer was cancelled";
            x.reasons = {"You cancelled the transfer. The file on the device can be incomplete. Send it again."};
            x.fallbacks = {kDownload};
            break;
        case TransferErrorCode::Device:
            x.title = "The device reported an error";
            x.reasons = {e.message.empty() ? "The device returned an error." : e.message,
                         "Check that the SD card has free space and is not read-only. Then send the file again."};
            x.fallbacks = {kFolder, kDownload, kScreen};
            break;
        case TransferErrorCode::Unknown:
        default:
            x.title = "The transfer failed";
            x.reasons = {e.message.empty() ? "An unknown error occurred." : e.message};
            x.fallbacks = lan;
            break;
    }
    return x;
}

} // namespace ks::net
