// "Send a file" (net/file_send.h): OutputFile shape, folder handling, size
// refusal, the Bluetooth rule, the history record. A recording fake
// transport checks what the send path gets; the real Folder transport
// checks the end-to-end write.
#include "check.h"

#include "net/file_send.h"
#include "net/net.h"

#include <filesystem>

using namespace ks;
using namespace ks::net;
namespace fs = std::filesystem;

namespace {

// Records each send and answers like a device.
class FakeTransport : public Transport {
public:
    int calls = 0;
    DeviceProfile lastProfile;
    OutputFile lastFile;
    bool fail = false;
    TransferMethod method() const override { return TransferMethod::CrossPointHttp; }
    bool isSupported() const override { return true; }
    std::vector<std::string> limitations() const override { return {}; }
    ConnectionTestResult testConnection(const DeviceProfile&) override { return {}; }
    TransferResult send(const DeviceProfile& p, const OutputFile& f, const ProgressFn& onProgress,
                        const bool* cancel) override {
        ++calls;
        lastProfile = p;
        lastFile = f;
        TransferResult r;
        if (cancel && *cancel) {
            r.error = TransferError{TransferErrorCode::Cancelled, "Cancelled."};
            return r;
        }
        if (fail) {
            r.error = TransferError{TransferErrorCode::Network, "No route to host."};
            return r;
        }
        if (onProgress) onProgress(TransferProgress{TransferState::Complete, 1.0, f.bytes.size(), f.bytes.size(), ""});
        r.ok = true;
        r.bytes = f.bytes.size();
        r.remotePath = remoteFilePath(p.targetFolder, f.name);
        return r;
    }
};

DeviceProfile profile(TransferMethod m) {
    DeviceProfile p = defaultProfile(FirmwareType::CrossPoint);
    p.id = "dev1";
    p.name = "X4";
    p.method = m;
    p.host = "127.0.0.1";
    p.targetFolder = "/Books/K-rosspoint/";
    return p;
}

std::string pattern(size_t n, char seed) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>(seed + static_cast<char>(i * 7));
    return s;
}

void testHelpers() {
    CHECK(isArticleInputPath("a/b/page.HTML"));
    CHECK(isArticleInputPath("x.htm") && isArticleInputPath("x.md") && isArticleInputPath("x.txt"));
    CHECK(isArticleInputPath("x.epub") && isArticleInputPath("x.url"));
    CHECK(!isArticleInputPath("firmware.bin"));
    CHECK(!isArticleInputPath("photo.png"));
    CHECK(!isArticleInputPath("noext"));
    CHECK(isFirmwareName("FIRMWARE.BIN"));
    CHECK(!isFirmwareName("firmware.bin.txt"));
    CHECK_EQ(fileBaseName("C:\\dl\\firmware.bin"), std::string("firmware.bin"));
    CHECK_EQ(fileBaseName("/tmp/x/a.pdf"), std::string("a.pdf"));
    CHECK_EQ(mimeForName("firmware.bin"), std::string("application/octet-stream"));
    CHECK_EQ(mimeForName("b.epub"), std::string("application/epub+zip"));
    CHECK_EQ(mimeForName("c.PDF"), std::string("application/pdf"));
}

void testOutputFileShape() {
    OutputFile f = makeRawFile("firmware.bin", pattern(1000, 1));
    CHECK_EQ(f.name, std::string("firmware.bin"));
    CHECK_EQ(f.mime, std::string("application/octet-stream"));
    CHECK_EQ(f.bytes.size(), size_t(1000));
    CHECK(f.bytes == pattern(1000, 1));
    OutputFile e = makeRawFile("book.epub", "PK");
    CHECK(e.format == OutputFormat::Epub);
    CHECK_EQ(e.mime, std::string("application/epub+zip"));
    OutputFile t = makeRawFile("notes.txt", "hi");
    CHECK(t.format == OutputFormat::Txt);
}

void testNames() {
    CHECK(sendNameProblem("firmware.bin", TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem("", TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem("  ", TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem("a/b.bin", TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem("a\\b.bin", TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem("..", TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem("a\tb", TransferMethod::WebDav).empty());
    std::string longName(230, 'a');
    CHECK(sendNameProblem(longName, TransferMethod::WebDav).empty());
    CHECK(!sendNameProblem(longName, TransferMethod::Ble).empty());
    CHECK(!sendNameProblem(std::string(300, 'a'), TransferMethod::WebDav).empty());
}

void testSizeRules() {
    const uint64_t MB = 1024ull * 1024;
    CHECK(checkFileSend(TransferMethod::CrossPointHttp, "f.bin", 5 * MB).ok());
    CHECK(checkFileSend(TransferMethod::CrossPointHttp, "f.bin", 5 * MB).warnings.empty());
    CHECK(!checkFileSend(TransferMethod::CrossPointHttp, "f.bin", 0).ok());
    CHECK(checkFileSend(TransferMethod::WebDav, "f.bin", kMaxSendFileBytes).ok());
    CHECK(!checkFileSend(TransferMethod::WebDav, "f.bin", kMaxSendFileBytes + 1).ok());
    CHECK(!checkFileSend(TransferMethod::Folder, "f.bin", 300 * MB).ok());
    // Bluetooth: warn over 3 MB, refuse over 16 MB, Wi-Fi note for firmware.
    auto small = checkFileSend(TransferMethod::Ble, "a.epub", 1 * MB);
    CHECK(small.ok() && small.warnings.empty());
    auto mid = checkFileSend(TransferMethod::Ble, "a.epub", 4 * MB);
    CHECK(mid.ok() && mid.warnings.size() == 1);
    auto fw = checkFileSend(TransferMethod::Ble, "firmware.bin", 2 * MB);
    CHECK(fw.ok());
    bool note = false;
    for (auto& w : fw.warnings) note = note || w == kBleFirmwareNote;
    CHECK(note);
    auto big = checkFileSend(TransferMethod::Ble, "a.epub", 17 * MB);
    CHECK(!big.ok());
    CHECK(big.problems.size() == 1 && big.problems[0].find("16 MB") != std::string::npos);
    CHECK(!checkFileSend(TransferMethod::Ble, "a.epub", kMaxSendFileBytes + 1).ok());
}

void testFolderAndSend() {
    DeviceProfile p = profile(TransferMethod::CrossPointHttp);
    CHECK_EQ(withFolder(p, "").targetFolder, std::string("/Books/K-rosspoint/"));
    CHECK_EQ(withFolder(p, "/").targetFolder, std::string("/"));
    CHECK_EQ(withFolder(p, "  ").targetFolder, std::string("/Books/K-rosspoint/"));
    CHECK_EQ(withFolder(p, "Books//Inbox").targetFolder, std::string("/Books/Inbox/"));
    CHECK_EQ(withFolder(p, "\\fw\\").targetFolder, std::string("/fw/"));

    FakeTransport t;
    OutputFile f = makeRawFile("firmware.bin", pattern(4096, 3));
    int progress = 0;
    TransferResult r = sendRawFile(t, p, "/", f, [&](const TransferProgress&) { ++progress; });
    CHECK(r.ok);
    CHECK_EQ(t.calls, 1);
    CHECK_EQ(t.lastProfile.targetFolder, std::string("/"));
    CHECK_EQ(t.lastProfile.host, p.host);
    CHECK_EQ(t.lastFile.name, std::string("firmware.bin"));
    CHECK(t.lastFile.bytes == f.bytes);
    CHECK_EQ(r.remotePath, std::string("/firmware.bin"));
    CHECK(progress > 0);

    // Default folder.
    r = sendRawFile(t, p, "", f, nullptr);
    CHECK_EQ(r.remotePath, std::string("/Books/K-rosspoint/firmware.bin"));

    // Refused before the transport: empty file, bad name, Bluetooth > 16 MB.
    int before = t.calls;
    r = sendRawFile(t, p, "/", makeRawFile("empty.bin", ""), nullptr);
    CHECK(!r.ok && r.error && r.error->code == TransferErrorCode::Unsupported);
    r = sendRawFile(t, p, "/", makeRawFile("a/b.bin", "x"), nullptr);
    CHECK(!r.ok);
    DeviceProfile b = profile(TransferMethod::Ble);
    r = sendRawFile(t, b, "/", makeRawFile("big.bin", std::string(17u * 1024 * 1024, 'x')), nullptr);
    CHECK(!r.ok && r.error && r.error->message.find("16 MB") != std::string::npos);
    CHECK_EQ(t.calls, before);

    // Cancel and failure come back from the transport.
    bool cancel = true;
    r = sendRawFile(t, p, "/", f, nullptr, &cancel);
    CHECK(!r.ok && r.error && r.error->code == TransferErrorCode::Cancelled);
    t.fail = true;
    r = sendRawFile(t, p, "/", f, nullptr);
    CHECK(!r.ok && r.error && r.error->code == TransferErrorCode::Network);
}

void testFolderTransport() {
    // The real USB-folder transport: the file lands at <host>/<folder>/<name>.
    fs::path root = fs::u8path(dataDir()) / "usb";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    DeviceProfile p = profile(TransferMethod::Folder);
    p.host = root.u8string();
    std::string bytes = pattern(10000, 5);
    OutputFile f = makeRawFile("firmware.bin", bytes);
    TransferResult r = sendRawFile(transport(TransferMethod::Folder), p, "/", f, nullptr);
    CHECK(r.ok);
    bool ok = false;
    std::string back = readFile((root / "firmware.bin").u8string(), &ok);
    CHECK(ok && back == bytes);
    // Overwrite works the same as for an article.
    f.bytes = "new";
    r = sendRawFile(transport(TransferMethod::Folder), p, "/", f, nullptr);
    CHECK(r.ok);
    back = readFile((root / "firmware.bin").u8string(), &ok);
    CHECK(ok && back == "new");
    // A sub folder is made.
    r = sendRawFile(transport(TransferMethod::Folder), p, "fw/x", f, nullptr);
    CHECK(r.ok);
    CHECK(fs::exists(root / "fw" / "x" / "firmware.bin"));
}

void testHistory() {
    clearFileSends();
    CHECK(listFileSends().empty());
    DeviceProfile p = profile(TransferMethod::WebDav);
    OutputFile f = makeRawFile("firmware.bin", std::string(2048, 'x'));
    TransferResult ok;
    ok.ok = true;
    ok.bytes = 2048;
    ok.remotePath = "/firmware.bin";
    TransferRecord r = makeFileSendRecord(p, f, ok);
    CHECK(r.articleId.empty());
    CHECK_EQ(r.fileName, std::string("/firmware.bin"));
    CHECK_EQ(r.deviceId, std::string("dev1"));
    CHECK(r.method == TransferMethod::WebDav);
    CHECK(r.ok && r.bytes == 2048);
    addFileSend(r);
    TransferResult bad;
    bad.error = TransferError{TransferErrorCode::Network, "down"};
    TransferRecord r2 = makeFileSendRecord(p, f, bad);
    CHECK(!r2.ok && r2.error == "down" && r2.fileName == "firmware.bin");
    addFileSend(r2);
    auto list = listFileSends();
    CHECK_EQ(list.size(), size_t(2));
    CHECK(list.size() == 2 && list[0].id == r2.id && list[1].id == r.id);   // newest first
    CHECK(list.size() == 2 && list[1].fileName == "/firmware.bin" && list[1].articleId.empty());
    // No library article was made.
    CHECK(listEntries().empty());
    for (int i = 0; i < 210; ++i) addFileSend(r);
    CHECK_EQ(listFileSends().size(), size_t(200));
    clearFileSends();
}

} // namespace

int main() {
    testHelpers();
    testOutputFileShape();
    testNames();
    testSizeRules();
    testFolderAndSend();
    testFolderTransport();
    testHistory();
    TEST_MAIN_END();
}
