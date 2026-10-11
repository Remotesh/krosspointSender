// BLE transport (MOCK). No radio is used. send() simulates the pipeline in
// about 1.2 s and emits real progress events, so the UI can be built and
// tested now. The real adapter will replace the simulated steps with BLE
// calls when the K-Reader firmware ships the service below.
//
// ---------------- Future K-Reader BLE file transfer protocol (draft) ----------------
//
// Transport: BLE GATT. The reader is the peripheral. The computer is the
// central. All integers are little-endian. All strings are UTF-8.
//
// Service
//   KREADER_FILE_SERVICE   6b726472-0001-4b52-8000-00000000f11e   (placeholder, not final)
//
// Characteristics
//   METADATA  ...-0002  write (with response)
//       Payload: u8 version (=1) | u32 fileSize | u32 crc32 | u16 chunkSize |
//                u8 nameLen | name[nameLen] | u8 pathLen | path[pathLen]
//       path is the target folder. Default "/Books/K-rosspoint/".
//       The reader answers on STATUS with READY or ERROR.
//   CHUNK     ...-0003  write without response
//       Payload: u32 offset | data[<= 244 - 4]
//       244 bytes is the payload of a 247-byte ATT MTU. The client asks for
//       MTU 247. With a smaller MTU, chunk = MTU - 3 - 4 bytes of data.
//       The client sends chunks in order. It stops after each 32 chunks until
//       STATUS reports ACK for that offset (simple window flow control).
//   CONTROL   ...-0004  write (with response)
//       Opcodes: 0x01 BEGIN (after METADATA), 0x02 VERIFY, 0x03 COMMIT,
//                0x04 ABORT, 0x05 PING.
//   STATUS    ...-0005  notify
//       Payload: u8 code | u32 value | optional UTF-8 text
//       Codes: 0x00 READY, 0x01 ACK(offset received), 0x02 CRC_OK(crc32),
//              0x03 CRC_BAD(crc32 seen), 0x04 COMMITTED, 0x05 ERROR(text),
//              0x06 BUSY, 0x07 NO_SPACE.
//
// Sequence
//   1. Pair and connect. Subscribe to STATUS.
//   2. Write METADATA. Wait for READY.
//   3. CONTROL BEGIN. Send the CHUNK writes. Progress = last ACK offset / fileSize.
//   4. CONTROL VERIFY. The reader calculates CRC32 (IEEE 802.3, poly 0xEDB88320)
//      over the received bytes. CRC_OK continues. CRC_BAD starts again at step 2.
//   5. CONTROL COMMIT. The reader moves the temp file to <path><name>
//      (default /Books/K-rosspoint/<name>) and replies COMMITTED.
//   6. Disconnect. ABORT at any time deletes the temp file.
//
// Security: the reader accepts writes only while its "Receive via Bluetooth"
// screen is open. The link is local. No server is used.
#include "net/net_internal.h"

#include <nlohmann/json.hpp>

#include <cstdio>

namespace ks::net::detail {

namespace {

constexpr const char* kServiceUuid = "6b726472-0001-4b52-8000-00000000f11e";
constexpr size_t kBleChunk = 244;
constexpr const char* kDefaultFolder = "/Books/K-rosspoint/";
// Simulated step times in ms. Total about 1.2 s.
constexpr int kPair = 200, kMeta = 100, kChunks = 600, kChecksum = 200, kCommit = 100;
constexpr int kSteps = 16;

} // namespace

class BleMockTransport final : public Transport {
public:
    TransferMethod method() const override { return TransferMethod::Ble; }
    bool isSupported() const override { return true; }
    std::vector<std::string> limitations() const override {
        return {"Bluetooth transfer is a simulation. No data leaves this computer.",
                "It needs the future K-Reader firmware. CrossPoint has no Bluetooth file transfer."};
    }

    ConnectionTestResult testConnection(const DeviceProfile&) override {
        ConnectionTestResult res;
        sleepCancellable(kPair, nullptr);
        nlohmann::json info = {{"simulated", true}, {"service", kServiceUuid}, {"chunkSize", kBleChunk}};
        res.ok = true;
        res.latencyMs = kPair;
        res.info = info.dump();
        return res;
    }

    TransferResult send(const DeviceProfile& p, const OutputFile& file, const ProgressFn& onProgress,
                        const bool* cancel) override {
        return runSend(file, onProgress, [&]() -> std::string {
            Emitter emit(onProgress);
            if (isCancelled(cancel)) throw cancelled();
            const uint64_t total = file.bytes.size();
            const std::string folder = p.targetFolder.empty() ? kDefaultFolder : normalizeFolder(p.targetFolder);
            const uint64_t chunks = total == 0 ? 1 : (total + kBleChunk - 1) / kBleChunk;

            emit(TransferState::Connecting, "Pairing with the reader (simulated)");
            sleepCancellable(kPair, cancel);
            emit(TransferState::Connecting,
                 "Sending metadata: " + file.name + ", " + std::to_string(total) + " bytes (simulated)");
            sleepCancellable(kMeta, cancel);

            for (int i = 1; i <= kSteps; ++i) {
                sleepCancellable(kChunks / kSteps, cancel);
                uint64_t sent = total * static_cast<uint64_t>(i) / kSteps;
                uint64_t chunk = (chunks * static_cast<uint64_t>(i) + kSteps - 1) / kSteps;
                emit(TransferState::Uploading,
                     "Chunk " + std::to_string(chunk) + " of " + std::to_string(chunks) + " (simulated)",
                     static_cast<double>(i) / kSteps, sent, total);
            }

            char crcText[16];
            std::snprintf(crcText, sizeof crcText, "%08x", static_cast<unsigned>(crc32(file.bytes)));
            emit(TransferState::Verifying, std::string("Checking CRC32 ") + crcText + " (simulated)", 1, total, total);
            sleepCancellable(kChecksum, cancel);
            emit(TransferState::Verifying, "Committing to " + folder + " (simulated)", 1, total, total);
            sleepCancellable(kCommit, cancel);
            return remoteFilePath(folder, file.name);
        });
    }
};

Transport& bleMockTransport() {
    static BleMockTransport t;
    return t;
}

} // namespace ks::net::detail
