// CrossPoint BLE file transfer protocol v1, the sender side
// (docs/BLE_PROTOCOL.md at the repository root is the contract).
// Internal to src/net. Platform independent: it runs on any BleLink.
#pragma once

#include "net/ble_link.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace ks::net::ble {

constexpr uint8_t kProtocolVersion = 1;

// Senders refuse files over 16 MB before connecting and warn over 3 MB.
constexpr uint64_t kMaxFileBytes = 16ull * 1024 * 1024;
constexpr uint64_t kWarnFileBytes = 3ull * 1024 * 1024;
// At most 48 chunks outstanding beyond the last ACKed offset.
constexpr int kWindowChunks = 48;
// META name and path length limits (bytes).
constexpr size_t kMaxNameBytes = 200;
constexpr size_t kMaxPathBytes = 200;

enum Opcode : uint8_t { OpVerify = 0x02, OpCommit = 0x03, OpAbort = 0x04, OpPing = 0x05 };

enum StatusCode : uint8_t {
    StReady = 0x00,
    StAck = 0x01,
    StCrcOk = 0x02,
    StCrcBad = 0x03,
    StCommitted = 0x04,
    StError = 0x05,
    StBusy = 0x06,
    StNoSpace = 0x07,
    StAborted = 0x08,
    StPong = 0x09,
};

struct Status {
    uint8_t code = 0;
    uint32_t value = 0;
    std::string text;
};

struct Info {
    int protocolVersion = 0;
    int maxChunkData = 0;
    uint32_t freeKiB = 0;
    std::string firmware;
};

// ---- encoding (little-endian)
std::optional<Status> parseStatus(const std::string& packet);   // nullopt when shorter than 5 bytes
std::optional<Info> parseInfo(const std::string& bytes);        // nullopt when too short
std::string encodeMeta(uint32_t fileSize, uint32_t crc, const std::string& name, const std::string& path);
std::string encodeChunk(uint32_t offset, const char* data, size_t len);
const char* statusName(uint8_t code);

// ---- size rules
// The refusal (> 16 MB, or empty) as an error, or nullopt when the size is allowed.
std::optional<TransferError> sizeProblem(uint64_t bytes);
// A warning text for files over 3 MB, or "" when the size is small.
std::string sizeWarning(uint64_t bytes);
// The reason the name cannot go in META, or "" when it is valid.
std::string nameProblem(const std::string& name);

struct Timeouts {
    int readyMs = 15000;    // META -> READY (the reader may scan the card first)
    int ackMs = 10000;      // no ACK for this long while chunks are outstanding
    int replyMs = 10000;    // VERIFY -> CRC_OK/CRC_BAD, COMMIT -> COMMITTED, PING -> PONG
    int abortMs = 1000;     // best-effort wait for ABORTED after a failure
};

// Counters the unit test reads.
struct SessionStats {
    int chunksWritten = 0;
    int maxOutstanding = 0;   // highest number of chunks past the last ACK
    int chunkData = 0;        // maxChunkData from READY
    bool abortSent = false;
};

// One protocol session on a connected link. Not thread safe; one call at a time.
class Session {
public:
    explicit Session(BleLink& link, Timeouts t = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Subscribes STATUS, reads INFO, then META .. COMMIT. Returns the
    // COMMITTED path. Throws NetError. On a failure after META it sends
    // ABORT when the link is still up. folder is the target folder.
    std::string sendFile(const OutputFile& file, const std::string& folder, const ProgressFn& onProgress,
                         const bool* cancel);

    // Subscribes STATUS, reads INFO (when it can), then PING -> PONG.
    // Returns the INFO (protocolVersion from PONG when INFO failed).
    Info ping(const bool* cancel);

    const SessionStats& stats() const { return stats_; }

private:
    void subscribe();
    void onStatus(const std::string& packet);
    // Waits for the next status message. nullopt on timeout. Throws
    // cancelled() and the link-lost error.
    std::optional<Status> next(int timeoutMs, const bool* cancel);
    std::optional<Status> poll();   // non-blocking
    void clearQueue();
    void bestEffortAbort();
    [[noreturn]] void failOn(const Status& s, const char* during);

    BleLink& link_;
    Timeouts t_;
    SessionStats stats_;
    bool subscribed_ = false;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Status> queue_;
};

} // namespace ks::net::ble
