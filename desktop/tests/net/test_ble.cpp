// Unit test of the CrossPoint BLE protocol driver (src/net/ble_protocol.*)
// against a fake link that plays the reader (docs/BLE_PROTOCOL.md). No radio.
#include "check.h"
#include "net/ble_protocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

using namespace ks;
using namespace ks::net;
using namespace ks::net::ble;

namespace {

void putU32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
uint32_t getU32(const std::string& s, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
    return v;
}
std::string status(uint8_t code, uint32_t value, const std::string& text = {}) {
    std::string s(1, static_cast<char>(code));
    putU32(s, value);
    return s + text;
}

// The reader, simulated. ACKs go out on a delivery thread after ackDelayMs
// so the sender really has to wait on its window.
class FakeReader final : public BleLink {
public:
    enum class OnMeta { Ready, Silent, NoSpace, Busy, Error };

    // Scenario knobs.
    OnMeta onMeta = OnMeta::Ready;
    uint32_t readyChunk = 100;
    int ackDelayMs = 0;
    int stopAckAfterChunks = -1;   // stop ACKing after this many chunks
    int errorAtChunk = -1;         // send ERROR "Out of order" when this chunk arrives
    bool crcBad = false;
    int dropAtChunk = -1;          // the link drops at this chunk
    uint32_t freeKiB = 1000;

    // Observations.
    std::string meta;
    std::string data;              // received file bytes
    int chunks = 0;
    int maxOutstanding = 0;
    size_t maxChunkPayload = 0;
    size_t firstChunkPayload = 0;
    std::vector<uint8_t> controls;
    bool writeAfterMetaWithoutReady = false;
    std::atomic<uint32_t> deliveredAck{0};

    FakeReader() : worker_([this] { run(); }) {}
    ~FakeReader() override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            stop_ = true;
        }
        qcv_.notify_all();
        worker_.join();
    }

    void connect(uint64_t, int, const bool*) override { connected_ = true; }
    void subscribeStatus(StatusFn fn) override {
        std::lock_guard<std::mutex> lk(hmu_);
        handler_ = std::move(fn);
    }
    std::string readInfo() override {
        std::string s(1, 1);
        s += static_cast<char>(200);   // INFO says 200; READY decides (readyChunk)
        s += static_cast<char>(0);
        putU32(s, freeKiB);
        std::string fw = "1.6.5-x4pro";
        s += static_cast<char>(fw.size());
        return s + fw;
    }
    void writeMeta(const std::string& bytes) override {
        alive();
        meta = bytes;
        data.clear();
        fileSize_ = getU32(bytes, 1);
        crc_ = getU32(bytes, 5);
        switch (onMeta) {
            case OnMeta::Ready: send(status(StReady, readyChunk)); ready_ = true; break;
            case OnMeta::Silent: break;
            case OnMeta::NoSpace: send(status(StNoSpace, freeKiB)); break;
            case OnMeta::Busy: send(status(StBusy, 0)); break;
            case OnMeta::Error: send(status(StError, 0, "Protected path")); break;
        }
    }
    void writeControl(uint8_t op) override {
        alive();
        controls.push_back(op);
        if (op == OpVerify) {
            uint32_t c = crc32(data);
            if (data.size() != fileSize_) send(status(StError, 0, "Short file"));
            else send(status(crcBad || c != crc_ ? StCrcBad : StCrcOk, crcBad ? (c ^ 1) : c));
        } else if (op == OpCommit) {
            std::string name = meta.substr(10, static_cast<unsigned char>(meta[9]));
            size_t pl = static_cast<unsigned char>(meta[10 + name.size()]);
            std::string path = meta.substr(11 + name.size(), pl);
            send(status(StCommitted, fileSize_, path + name));
        } else if (op == OpAbort) {
            ready_ = false;
            send(status(StAborted, static_cast<uint32_t>(data.size())));
        } else if (op == OpPing) {
            send(status(StPong, 1));
        } else {
            send(status(StError, 0, "Bad opcode"));
        }
    }
    void writeChunk(const std::string& bytes) override {
        alive();
        if (!ready_) writeAfterMetaWithoutReady = true;
        if (chunks == dropAtChunk) {
            connected_ = false;
            throw detail::NetError(TransferErrorCode::Network, "Bluetooth: writing CHUNK failed (the reader is unreachable).");
        }
        uint32_t off = getU32(bytes, 0);
        size_t n = bytes.size() - 4;
        if (!chunks) firstChunkPayload = n;
        maxChunkPayload = std::max(maxChunkPayload, n);
        ++chunks;
        int outstanding = chunks - static_cast<int>(deliveredAck.load() / readyChunk);
        maxOutstanding = std::max(maxOutstanding, outstanding);
        if (chunks - 1 == errorAtChunk || off != data.size()) {
            ready_ = false;
            send(status(StError, 0, "Out of order"));
            return;
        }
        data.append(bytes, 4, n);
        bool ackNow = (chunks % 16 == 0) || data.size() == fileSize_;
        if (ackNow && (stopAckAfterChunks < 0 || chunks <= stopAckAfterChunks))
            send(status(StAck, static_cast<uint32_t>(data.size())), ackDelayMs);
    }
    void disconnect() noexcept override { connected_ = false; }
    bool connected() const override { return connected_; }
    int mtu() const override { return 247; }

private:
    void alive() {
        if (!connected_) throw detail::NetError(TransferErrorCode::Network, "not connected");
    }
    void send(const std::string& packet, int delayMs = 0) {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            q_.push_back({std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs), packet});
        }
        qcv_.notify_all();
    }
    void run() {
        std::unique_lock<std::mutex> lk(qmu_);
        while (!stop_) {
            if (q_.empty()) {
                qcv_.wait(lk);
                continue;
            }
            auto at = q_.front().first;
            if (std::chrono::steady_clock::now() < at) {
                qcv_.wait_until(lk, at);
                continue;
            }
            std::string p = q_.front().second;
            q_.pop_front();
            lk.unlock();
            if (static_cast<uint8_t>(p[0]) == StAck) deliveredAck = getU32(p, 1);
            {
                std::lock_guard<std::mutex> hl(hmu_);
                if (handler_) handler_(p);
            }
            lk.lock();
        }
    }

    std::atomic<bool> connected_{false};
    bool ready_ = false;
    uint32_t fileSize_ = 0, crc_ = 0;
    std::mutex hmu_;
    StatusFn handler_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::string>> q_;
    bool stop_ = false;
    std::thread worker_;
};

OutputFile makeFile(const std::string& name, size_t n) {
    OutputFile f;
    f.name = name;
    f.bytes.resize(n);
    for (size_t i = 0; i < n; ++i) f.bytes[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    return f;
}

Timeouts fast() {
    Timeouts t;
    t.readyMs = 300;
    t.ackMs = 300;
    t.replyMs = 500;
    t.abortMs = 300;
    return t;
}

// Runs one send. Returns the error code (or nullopt on success) and the path.
struct Outcome {
    std::optional<TransferErrorCode> code;
    std::string message;
    std::string path;
    SessionStats stats;
    std::vector<TransferProgress> events;
};

Outcome run(FakeReader& r, const OutputFile& f, const bool* cancel = nullptr, Timeouts t = fast(),
            std::function<void(const TransferProgress&)> extra = nullptr) {
    Outcome o;
    r.connect(1, 1000, nullptr);
    Session s(r, t);
    ProgressFn fn = [&](const TransferProgress& p) {
        o.events.push_back(p);
        if (extra) extra(p);
    };
    try {
        o.path = s.sendFile(f, "/Books/K-rosspoint/", fn, cancel);
    } catch (const detail::NetError& e) {
        o.code = e.err.code;
        o.message = e.err.message;
    }
    o.stats = s.stats();
    return o;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
bool sentAbort(const FakeReader& r) {
    return std::find(r.controls.begin(), r.controls.end(), static_cast<uint8_t>(OpAbort)) != r.controls.end();
}

// ------------------------------------------------------------- tests

void testEncoding() {
    CHECK_EQ(crc32("123456789"), 0xCBF43926u);
    std::string m = encodeMeta(0x01020304u, 0xCBF43926u, "a.epub", "/Books/");
    CHECK_EQ(m.size(), size_t(1 + 4 + 4 + 1 + 6 + 1 + 7));
    CHECK_EQ(static_cast<int>(m[0]), 1);
    CHECK_EQ(getU32(m, 1), 0x01020304u);
    CHECK_EQ(static_cast<unsigned char>(m[1]), 0x04);   // little-endian
    CHECK_EQ(getU32(m, 5), 0xCBF43926u);
    CHECK_EQ(static_cast<int>(m[9]), 6);
    CHECK_EQ(m.substr(10, 6), std::string("a.epub"));
    CHECK_EQ(static_cast<int>(m[16]), 7);
    CHECK_EQ(m.substr(17), std::string("/Books/"));

    std::string c = encodeChunk(0x10203040u, "xyz", 3);
    CHECK_EQ(c.size(), size_t(7));
    CHECK_EQ(getU32(c, 0), 0x10203040u);
    CHECK_EQ(c.substr(4), std::string("xyz"));

    auto st = parseStatus(status(StCommitted, 42, "/Books/a.epub"));
    CHECK(st.has_value());
    if (st) {
        CHECK_EQ(st->code, static_cast<uint8_t>(StCommitted));
        CHECK_EQ(st->value, 42u);
        CHECK_EQ(st->text, std::string("/Books/a.epub"));
    }
    CHECK(!parseStatus("abcd").has_value());
    CHECK_EQ(std::string(statusName(StNoSpace)), std::string("NO_SPACE"));

    FakeReader r;
    auto info = parseInfo(r.readInfo());
    CHECK(info.has_value());
    if (info) {
        CHECK_EQ(info->protocolVersion, 1);
        CHECK_EQ(info->maxChunkData, 200);
        CHECK_EQ(info->freeKiB, 1000u);
        CHECK_EQ(info->firmware, std::string("1.6.5-x4pro"));
    }
    CHECK(!parseInfo("short").has_value());

    CHECK(nameProblem("a.epub").empty());
    CHECK(!nameProblem("").empty());
    CHECK(!nameProblem(".hidden").empty());
    CHECK(!nameProblem("a/b.epub").empty());
    CHECK(!nameProblem("a\\b.epub").empty());
    CHECK(!nameProblem("..").empty());
    CHECK(!nameProblem(std::string(201, 'a')).empty());

    CHECK(!sizeProblem(16ull * 1024 * 1024).has_value());
    CHECK(sizeProblem(16ull * 1024 * 1024 + 1).has_value());
    CHECK(sizeProblem(0).has_value());
    CHECK(sizeWarning(3ull * 1024 * 1024).empty());
    CHECK(!sizeWarning(3ull * 1024 * 1024 + 1).empty());

    CHECK_EQ(formatBleAddress(0xC0FFEE123A7Full), std::string("C0:FF:EE:12:3A:7F"));
    CHECK(parseBleAddress("C0:FF:EE:12:3A:7F") == std::optional<uint64_t>(0xC0FFEE123A7Full));
    CHECK(parseBleAddress("c0-ff-ee-12-3a-7f") == std::optional<uint64_t>(0xC0FFEE123A7Full));
    CHECK(parseBleAddress("c0ffee123a7f") == std::optional<uint64_t>(0xC0FFEE123A7Full));
    CHECK(!parseBleAddress("CrossPoint-3A7F").has_value());
    CHECK(!parseBleAddress("00:00:00:00:00:00").has_value());
    CHECK(!parseBleAddress("C0:FF:EE:12:3A").has_value());
    CHECK_EQ(fallbackName(0xC0FFEE123A7Full), std::string("CrossPoint-3A7F"));
}

void testHappyPath() {
    FakeReader r;
    r.ackDelayMs = 5;
    OutputFile f = makeFile("book.epub", 25000);   // 250 chunks of 100
    Outcome o = run(r, f);
    CHECK(!o.code.has_value());
    if (o.code) std::fprintf(stderr, "happy: %s\n", o.message.c_str());
    CHECK_EQ(o.path, std::string("/Books/K-rosspoint/book.epub"));
    CHECK(r.data == f.bytes);
    CHECK_EQ(r.chunks, 250);
    CHECK_EQ(o.stats.chunksWritten, 250);
    CHECK(!r.writeAfterMetaWithoutReady);
    CHECK(r.controls.size() == 2 && r.controls[0] == OpVerify && r.controls[1] == OpCommit);
    CHECK(!o.stats.abortSent);
    // META fields.
    CHECK_EQ(getU32(r.meta, 1), 25000u);
    CHECK_EQ(getU32(r.meta, 5), crc32(f.bytes));
    // Progress: last ACK / fileSize, monotonic, reaches 1.
    double last = -1;
    bool mono = true, sawFull = false, sawVerify = false;
    for (auto& e : o.events) {
        if (e.state == TransferState::Uploading && e.fraction >= 0) {
            if (e.fraction < last) mono = false;
            last = e.fraction;
            if (e.fraction == 1.0 && e.bytesSent == 25000) sawFull = true;
        }
        if (e.state == TransferState::Verifying) sawVerify = true;
    }
    CHECK(mono);
    CHECK(sawFull);
    CHECK(sawVerify);
}

void testWindow() {
    // Slow ACKs: the sender must stop at 48 outstanding chunks.
    FakeReader r;
    r.ackDelayMs = 40;
    OutputFile f = makeFile("w.epub", 300 * 100 + 17);   // 301 chunks, the last one short
    Outcome o = run(r, f);
    CHECK(!o.code.has_value());
    CHECK(r.data == f.bytes);
    CHECK(r.maxOutstanding <= kWindowChunks);
    CHECK(o.stats.maxOutstanding <= kWindowChunks);
    CHECK(o.stats.maxOutstanding >= 32);   // the window was really used
    std::printf("window: max outstanding reader %d, sender %d\n", r.maxOutstanding, o.stats.maxOutstanding);
}

void testChunkSizeFromReady() {
    FakeReader r;
    r.readyChunk = 37;   // INFO says 200; READY wins
    OutputFile f = makeFile("c.epub", 1000);
    Outcome o = run(r, f);
    CHECK(!o.code.has_value());
    CHECK_EQ(o.stats.chunkData, 37);
    CHECK_EQ(r.firstChunkPayload, size_t(37));
    CHECK_EQ(r.maxChunkPayload, size_t(37));
    CHECK_EQ(r.chunks, 28);   // ceil(1000 / 37)
    CHECK(r.data == f.bytes);
}

void testOutOfOrderError() {
    FakeReader r;
    r.errorAtChunk = 20;
    Outcome o = run(r, makeFile("e.epub", 10000));
    CHECK(o.code == TransferErrorCode::Device);
    CHECK(contains(o.message, "Out of order"));
    CHECK(o.stats.abortSent);
    CHECK(sentAbort(r));
    CHECK(o.events.back().state != TransferState::Complete);
}

void testCrcBad() {
    FakeReader r;
    r.crcBad = true;
    Outcome o = run(r, makeFile("crc.epub", 5000));
    CHECK(o.code == TransferErrorCode::Device);
    CHECK(contains(o.message, "CRC"));
    CHECK(r.controls.size() == 2 && r.controls[0] == OpVerify && r.controls[1] == OpAbort);   // no COMMIT
    CHECK(o.stats.abortSent);
}

void testNoSpaceBusyError() {
    {
        FakeReader r;
        r.onMeta = FakeReader::OnMeta::NoSpace;
        r.freeKiB = 3;
        Outcome o = run(r, makeFile("n.epub", 5000));
        CHECK(o.code == TransferErrorCode::Device);
        CHECK(contains(o.message, "3 KiB"));
        CHECK_EQ(r.chunks, 0);
    }
    {
        FakeReader r;
        r.onMeta = FakeReader::OnMeta::Busy;
        Outcome o = run(r, makeFile("b.epub", 5000));
        CHECK(o.code == TransferErrorCode::Device);
        CHECK(contains(o.message, "busy"));
    }
    {
        FakeReader r;
        r.onMeta = FakeReader::OnMeta::Error;
        Outcome o = run(r, makeFile("p.epub", 5000));
        CHECK(o.code == TransferErrorCode::Device);
        CHECK(contains(o.message, "Protected path"));
    }
}

void testReadyTimeout() {
    FakeReader r;
    r.onMeta = FakeReader::OnMeta::Silent;
    auto t0 = std::chrono::steady_clock::now();
    Outcome o = run(r, makeFile("t.epub", 5000));
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(o.code == TransferErrorCode::Network);
    CHECK_EQ(r.chunks, 0);
    CHECK(o.stats.abortSent);   // a failure after META sends ABORT
    CHECK(ms >= 280 && ms < 3000);
    CHECK_EQ(Timeouts{}.readyMs, 15000);  // the spec values
    CHECK_EQ(Timeouts{}.ackMs, 10000);
}

void testAckTimeout() {
    FakeReader r;
    r.stopAckAfterChunks = 16;
    Outcome o = run(r, makeFile("a.epub", 20000));   // 200 chunks
    CHECK(o.code == TransferErrorCode::Network);
    CHECK(contains(o.message, "ACK"));
    CHECK(r.chunks <= 16 + kWindowChunks);   // stopped at the window
    CHECK(r.maxOutstanding <= kWindowChunks);
    CHECK(o.stats.abortSent);
}

void testSizeRefusal() {
    FakeReader r;
    Outcome o = run(r, makeFile("big.epub", 16 * 1024 * 1024 + 1));
    CHECK(o.code == TransferErrorCode::Unsupported);
    CHECK(r.meta.empty());
    CHECK_EQ(r.chunks, 0);
    CHECK(r.controls.empty());
    FakeReader r2;
    Outcome o2 = run(r2, makeFile(".hidden.epub", 10));
    CHECK(o2.code.has_value());
    CHECK(r2.meta.empty());
}

void testCancel() {
    FakeReader r;
    r.ackDelayMs = 20;
    bool cancel = false;
    Outcome o = run(r, makeFile("x.epub", 30000), &cancel, fast(), [&](const TransferProgress& p) {
        if (p.state == TransferState::Uploading && p.bytesSent > 0) cancel = true;
    });
    CHECK(o.code == TransferErrorCode::Cancelled);
    CHECK(r.chunks < 300);
    CHECK(sentAbort(r));
    // Cancel before META: nothing is written.
    FakeReader r2;
    bool pre = true;
    Outcome o2 = run(r2, makeFile("y.epub", 3000), &pre);
    CHECK(o2.code == TransferErrorCode::Cancelled);
    CHECK(r2.meta.empty());
}

void testLinkLost() {
    FakeReader r;
    r.dropAtChunk = 10;
    Outcome o = run(r, makeFile("l.epub", 5000));
    CHECK(o.code == TransferErrorCode::Network);
    CHECK(!sentAbort(r));   // the link is down: no ABORT
}

void testPing() {
    FakeReader r;
    r.connect(1, 1000, nullptr);
    Session s(r, fast());
    Info i = s.ping(nullptr);
    CHECK_EQ(i.protocolVersion, 1);
    CHECK_EQ(i.firmware, std::string("1.6.5-x4pro"));
    CHECK(!r.controls.empty() && r.controls.back() == OpPing);
}

void testProfileStore() {
    DeviceProfile p = defaultProfile(FirmwareType::CrossPoint);
    p.name = "X4 BLE";
    p.host = "";
    p.method = TransferMethod::Ble;
    p.bleAddress = 0xC0FFEE123A7Full;
    p.bleName = "CrossPoint-3A7F";
    CHECK(validateProfile(p).empty());
    DeviceProfile saved = saveProfile(p);
    auto back = getProfile(saved.id);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->method == TransferMethod::Ble);
        CHECK_EQ(back->bleAddress, 0xC0FFEE123A7Full);
        CHECK_EQ(back->bleName, std::string("CrossPoint-3A7F"));
    }
    deleteProfile(saved.id);
}

} // namespace

int main() {
    testEncoding();
    testHappyPath();
    testWindow();
    testChunkSizeFromReady();
    testOutOfOrderError();
    testCrcBad();
    testNoSpaceBusyError();
    testReadyTimeout();
    testAckTimeout();
    testSizeRefusal();
    testCancel();
    testLinkLost();
    testPing();
    testProfileStore();
    TEST_MAIN_END();
}
