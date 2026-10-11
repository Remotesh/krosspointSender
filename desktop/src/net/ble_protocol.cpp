// CrossPoint BLE file transfer protocol v1, sender side. See ble_protocol.h
// and docs/BLE_PROTOCOL.md.
#include "net/ble_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace ks::net::ble {

using detail::NetError;
using Clock = std::chrono::steady_clock;

namespace {

void putU32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF);
}

uint32_t getU32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
    return v;
}

std::string mb(uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", static_cast<unsigned>(v));
    return buf;
}

int msLeft(Clock::time_point deadline) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return ms < 0 ? 0 : static_cast<int>(ms);
}

NetError lostError() {
    return NetError(TransferErrorCode::Network,
                    "The Bluetooth connection to the reader was lost. Keep the reader on the Receive via Bluetooth "
                    "screen and near this computer.");
}

} // namespace

// ------------------------------------------------------------- encoding

std::optional<Status> parseStatus(const std::string& p) {
    if (p.size() < 5) return std::nullopt;
    Status s;
    s.code = static_cast<uint8_t>(p[0]);
    s.value = getU32(p, 1);
    s.text = p.substr(5);
    return s;
}

std::optional<Info> parseInfo(const std::string& b) {
    if (b.size() < 8) return std::nullopt;
    Info i;
    i.protocolVersion = static_cast<unsigned char>(b[0]);
    i.maxChunkData = static_cast<unsigned char>(b[1]) | (static_cast<unsigned char>(b[2]) << 8);
    i.freeKiB = getU32(b, 3);
    size_t n = static_cast<unsigned char>(b[7]);
    i.firmware = b.substr(8, std::min(n, b.size() - 8));
    return i;
}

std::string encodeMeta(uint32_t fileSize, uint32_t crc, const std::string& name, const std::string& path) {
    std::string out;
    out += static_cast<char>(kProtocolVersion);
    putU32(out, fileSize);
    putU32(out, crc);
    out += static_cast<char>(static_cast<uint8_t>(name.size()));
    out += name;
    out += static_cast<char>(static_cast<uint8_t>(path.size()));
    out += path;
    return out;
}

std::string encodeChunk(uint32_t offset, const char* data, size_t len) {
    std::string out;
    out.reserve(4 + len);
    putU32(out, offset);
    out.append(data, len);
    return out;
}

const char* statusName(uint8_t code) {
    switch (code) {
        case StReady: return "READY";
        case StAck: return "ACK";
        case StCrcOk: return "CRC_OK";
        case StCrcBad: return "CRC_BAD";
        case StCommitted: return "COMMITTED";
        case StError: return "ERROR";
        case StBusy: return "BUSY";
        case StNoSpace: return "NO_SPACE";
        case StAborted: return "ABORTED";
        case StPong: return "PONG";
    }
    return "UNKNOWN";
}

// ------------------------------------------------------------- size and name rules

std::optional<TransferError> sizeProblem(uint64_t bytes) {
    if (bytes == 0) return detail::makeError(TransferErrorCode::Unsupported, "The file is empty. Bluetooth transfer needs at least one byte.");
    if (bytes > kMaxFileBytes)
        return detail::makeError(TransferErrorCode::Unsupported,
                                 "The file is " + mb(bytes) + ". Bluetooth transfer allows at most 16 MB. Use Wi-Fi or the USB folder method.");
    return std::nullopt;
}

std::string sizeWarning(uint64_t bytes) {
    if (bytes <= kWarnFileBytes) return {};
    return "The file is " + mb(bytes) + ". Bluetooth sends 10 to 50 KB/s, so this can take " +
           std::to_string(std::max<uint64_t>(1, bytes / (20 * 1024) / 60)) + " minutes or more.";
}

std::string nameProblem(const std::string& name) {
    if (name.empty()) return "The file name is empty.";
    if (name.size() > kMaxNameBytes) return "The file name is longer than 200 bytes.";
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
        return "The file name must not contain / or \\.";
    if (name == "." || name == "..") return "The file name must not be . or ..";
    if (name[0] == '.') return "The file name must not start with a dot.";
    return {};
}

// ------------------------------------------------------------- session

Session::Session(BleLink& link, Timeouts t) : link_(link), t_(t) {}

Session::~Session() {
    // Detach the handler so no notification reaches a destroyed session.
    if (subscribed_) {
        try {
            link_.subscribeStatus({});
        } catch (...) {
        }
    }
}

void Session::subscribe() {
    if (subscribed_) return;
    link_.subscribeStatus([this](const std::string& packet) { onStatus(packet); });
    subscribed_ = true;
}

void Session::onStatus(const std::string& packet) {
    auto s = parseStatus(packet);
    if (!s) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(*s);
    }
    cv_.notify_all();
}

void Session::clearQueue() {
    std::lock_guard<std::mutex> lk(mu_);
    queue_.clear();
}

std::optional<Status> Session::poll() {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.empty()) return std::nullopt;
    Status s = queue_.front();
    queue_.pop_front();
    return s;
}

std::optional<Status> Session::next(int timeoutMs, const bool* cancel) {
    auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(mu_);
    for (;;) {
        if (!queue_.empty()) {
            Status s = queue_.front();
            queue_.pop_front();
            return s;
        }
        if (detail::isCancelled(cancel)) throw detail::cancelled();
        if (!link_.connected()) throw lostError();
        auto now = Clock::now();
        if (now >= deadline) return std::nullopt;
        cv_.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(20)));
    }
}

void Session::failOn(const Status& s, const char* during) {
    switch (s.code) {
        case StError:
            throw NetError(TransferErrorCode::Device,
                           std::string("The reader reported an error ") + during + ": " +
                               (s.text.empty() ? std::string("no reason given") : s.text) + ".");
        case StBusy:
            throw NetError(TransferErrorCode::Device, "The reader is busy with another transfer. Try again in a moment.");
        case StNoSpace:
            throw NetError(TransferErrorCode::Device,
                           "The SD card is too full: the reader has " + std::to_string(s.value) + " KiB free.");
        default:
            throw NetError(TransferErrorCode::Device,
                           std::string("The reader sent an unexpected ") + statusName(s.code) + " " + during + ".");
    }
}

void Session::bestEffortAbort() {
    if (!link_.connected()) return;
    try {
        clearQueue();
        link_.writeControl(OpAbort);
        stats_.abortSent = true;
        auto deadline = Clock::now() + std::chrono::milliseconds(t_.abortMs);
        while (auto m = next(msLeft(deadline), nullptr))
            if (m->code == StAborted || m->code == StError) break;
    } catch (...) {
        // Best effort only. The reader also deletes the .part file on disconnect.
    }
}

namespace {

// True for the statuses after which the reader has closed the session.
bool isTerminalError(uint8_t code) { return code == StError || code == StBusy || code == StNoSpace; }

} // namespace

std::string Session::sendFile(const OutputFile& file, const std::string& folder, const ProgressFn& onProgress,
                              const bool* cancel) {
    detail::Emitter emit(onProgress);
    const uint64_t total = file.bytes.size();
    if (auto e = sizeProblem(total)) throw NetError(*e);
    if (std::string np = nameProblem(file.name); !np.empty())
        throw NetError(TransferErrorCode::Device, "This file name cannot go over Bluetooth: " + np);
    const std::string path = normalizeFolder(folder);
    if (path.size() > kMaxPathBytes) throw NetError(TransferErrorCode::Device, "The target folder is longer than 200 bytes.");
    const uint32_t crc = crc32(file.bytes);

    subscribe();
    std::optional<Info> info;
    try {
        info = parseInfo(link_.readInfo());
    } catch (const NetError&) {
        if (!link_.connected()) throw;   // INFO is optional; a dead link is not
    }
    if (info) {
        if (info->protocolVersion != kProtocolVersion)
            throw NetError(TransferErrorCode::Unsupported,
                           "The reader uses Bluetooth protocol version " + std::to_string(info->protocolVersion) +
                               ". This app uses version 1. Update the app or the reader firmware.");
        std::string line = "Reader " + (info->firmware.empty() ? std::string("connected") : info->firmware);
        if (info->freeKiB) line += ", " + mb(static_cast<uint64_t>(info->freeKiB) * 1024) + " free";
        emit(TransferState::Connecting, line);
    }
    if (detail::isCancelled(cancel)) throw detail::cancelled();

    clearQueue();
    emit(TransferState::Connecting, "Starting the session (" + file.name + ", CRC " + hex32(crc) + ")");
    link_.writeMeta(encodeMeta(static_cast<uint32_t>(total), crc, file.name, path));

    try {
        // ---- READY within readyMs
        uint32_t chunk = 0;
        {
            auto deadline = Clock::now() + std::chrono::milliseconds(t_.readyMs);
            for (;;) {
                auto m = next(msLeft(deadline), cancel);
                if (!m)
                    throw NetError(TransferErrorCode::Network,
                                   "The reader did not answer the start of the transfer within " +
                                       std::to_string(t_.readyMs / 1000) + " s.");
                if (m->code == StReady) {
                    chunk = m->value;
                    break;
                }
                if (isTerminalError(m->code)) failOn(*m, "at the start");
                // Ignore anything else (a late PONG or ACK).
            }
        }
        if (chunk < 1 || chunk > 512)
            throw NetError(TransferErrorCode::Device, "The reader asked for an invalid chunk size (" + std::to_string(chunk) + ").");
        stats_.chunkData = static_cast<int>(chunk);

        // ---- CHUNKs with the 48-chunk window, ACK every 16
        const int totalChunks = static_cast<int>((total + chunk - 1) / chunk);
        uint64_t sent = 0, acked = 0;
        int chunksSent = 0;
        auto ackClock = Clock::now();
        bool waiting = false;
        auto outstanding = [&] {
            int ackedChunks = acked >= total ? totalChunks : static_cast<int>(acked / chunk);
            return chunksSent - ackedChunks;
        };
        auto handle = [&](const Status& m) {
            if (m.code == StAck) {
                if (m.value > acked) {
                    acked = std::min<uint64_t>(m.value, total);
                    ackClock = Clock::now();
                    emit(TransferState::Uploading,
                         "Sent " + std::to_string(acked) + " of " + std::to_string(total) + " bytes",
                         static_cast<double>(acked) / static_cast<double>(total), acked, total);
                }
            } else if (isTerminalError(m.code)) {
                failOn(m, "during the upload");
            }
        };
        auto waitAck = [&] {
            if (!waiting) {
                waiting = true;
                ackClock = Clock::now();
            }
            auto deadline = ackClock + std::chrono::milliseconds(t_.ackMs);
            auto m = next(msLeft(deadline), cancel);
            if (m) {
                handle(*m);
            } else if (Clock::now() >= ackClock + std::chrono::milliseconds(t_.ackMs)) {
                throw NetError(TransferErrorCode::Network,
                               "The reader stopped confirming data (no ACK for " + std::to_string(t_.ackMs / 1000) +
                                   " s) at " + std::to_string(acked) + " of " + std::to_string(total) + " bytes.");
            }
        };

        emit(TransferState::Uploading,
             std::to_string(totalChunks) + " chunks of " + std::to_string(chunk) + " bytes", 0, 0, total);
        while (sent < total) {
            while (auto m = poll()) handle(*m);
            if (detail::isCancelled(cancel)) throw detail::cancelled();
            if (!link_.connected()) throw lostError();
            if (outstanding() >= kWindowChunks) {
                waitAck();
                continue;
            }
            waiting = false;
            size_t len = static_cast<size_t>(std::min<uint64_t>(chunk, total - sent));
            link_.writeChunk(encodeChunk(static_cast<uint32_t>(sent), file.bytes.data() + sent, len));
            sent += len;
            ++chunksSent;
            stats_.chunksWritten = chunksSent;
            stats_.maxOutstanding = std::max(stats_.maxOutstanding, outstanding());
        }
        while (acked < total) waitAck();

        // ---- VERIFY
        emit(TransferState::Verifying, "Checking CRC32 " + hex32(crc), 1, total, total);
        clearQueue();
        link_.writeControl(OpVerify);
        {
            auto deadline = Clock::now() + std::chrono::milliseconds(t_.replyMs);
            for (;;) {
                auto m = next(msLeft(deadline), cancel);
                if (!m) throw NetError(TransferErrorCode::Network, "The reader did not answer the CRC check.");
                if (m->code == StCrcOk) {
                    if (m->value != crc)
                        throw NetError(TransferErrorCode::Device, "CRC mismatch: the reader computed " + hex32(m->value) +
                                                                      ", the file has " + hex32(crc) + ".");
                    break;
                }
                if (m->code == StCrcBad)
                    throw NetError(TransferErrorCode::Device, "CRC mismatch: the reader computed " + hex32(m->value) +
                                                                  ", the file has " + hex32(crc) + ". Send the file again.");
                if (isTerminalError(m->code)) failOn(*m, "at the CRC check");
            }
        }

        // ---- COMMIT
        emit(TransferState::Verifying, "Saving the file on the reader", 1, total, total);
        link_.writeControl(OpCommit);
        auto deadline = Clock::now() + std::chrono::milliseconds(t_.replyMs);
        for (;;) {
            auto m = next(msLeft(deadline), cancel);
            if (!m) throw NetError(TransferErrorCode::Network, "The reader did not confirm the saved file.");
            if (m->code == StCommitted) return m->text.empty() ? remoteFilePath(path, file.name) : m->text;
            if (isTerminalError(m->code)) failOn(*m, "while saving");
        }
    } catch (...) {
        bestEffortAbort();
        throw;
    }
}

Info Session::ping(const bool* cancel) {
    subscribe();
    std::optional<Info> info;
    try {
        info = parseInfo(link_.readInfo());
    } catch (const NetError&) {
        if (!link_.connected()) throw;
    }
    clearQueue();
    link_.writeControl(OpPing);
    auto deadline = Clock::now() + std::chrono::milliseconds(t_.replyMs);
    for (;;) {
        auto m = next(msLeft(deadline), cancel);
        if (!m) throw NetError(TransferErrorCode::Network, "The reader did not answer PING.");
        if (m->code == StPong) {
            Info out = info.value_or(Info{});
            if (!info) out.protocolVersion = static_cast<int>(m->value);
            return out;
        }
        if (m->code == StError) failOn(*m, "to PING");
    }
}

} // namespace ks::net::ble
