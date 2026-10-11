// CLI mode. Runs without a window and prints the result.
//   krosspoint_sender --send <file> --host <ip> [--port 80] [--folder /Books/K-rosspoint/] [--method webdav|http|ws|folder]
//   krosspoint_sender --send <file> --method ble [--ble <address or name>] [--folder /Books/K-rosspoint/]
//   krosspoint_sender --discover [--timeout 2000] | --discover-ble
//   krosspoint_sender --convert <url-or-file> --out <file.epub>
//   krosspoint_sender --pack <wikipedia-url-or-title> --out <file.epub> [--depth 1] [--max 20] [--lang en]
//   krosspoint_sender --help
#include "app/app.h"
#include "spider/spider.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>

namespace ks::app {

namespace {

const char* const kUsage =
    "k-rosspointSender desktop 0.1\n"
    "\n"
    "Usage:\n"
    "  krosspoint_sender                      Start the window.\n"
    "  krosspoint_sender --send <file> --host <ip> [--port 80]\n"
    "                    [--folder /Books/K-rosspoint/] [--method webdav|http|ws|folder]\n"
    "                    [--user <name> --password <pw>]\n"
    "                                         Send a file to a CrossPoint reader.\n"
    "  krosspoint_sender --send <file> --method ble [--ble <address or name>]\n"
    "                    [--folder /Books/K-rosspoint/]\n"
    "                                         Send over Bluetooth LE (Windows). Without\n"
    "                                         --ble it scans and uses the only reader.\n"
    "                                         On the reader open File Transfer >\n"
    "                                         Receive via Bluetooth.\n"
    "  krosspoint_sender --discover [--timeout 2000]\n"
    "                                         Find readers on the LAN and over Bluetooth.\n"
    "  krosspoint_sender --discover-ble       Find readers over Bluetooth only.\n"
    "  krosspoint_sender --convert <url-or-file> --out <file.epub|.html|.txt|.md|.zip>\n"
    "                                         Fetch or read an article and write it.\n"
    "  krosspoint_sender --pack <wikipedia-url-or-title> --out <file.epub>\n"
    "                    [--depth 1] [--max 20] [--lang en]\n"
    "                                         Build a spider pack: the article and the\n"
    "                                         articles it links to, in one EPUB.\n"
    "                                         Send it after with --send.\n"
    "  krosspoint_sender --help               Show this text.\n"
    "\n"
    "Exit code: 0 on success, 1 on failure.\n";

using Args = std::map<std::string, std::string>;

// Parse "--key value" pairs. A flag with no value gets "1".
Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k.rfind("--", 0) != 0) {
            a["_extra"] = k;
            continue;
        }
        k = k.substr(2);
        if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) a[k] = argv[++i];
        else a[k] = "1";
    }
    return a;
}

std::string extOf(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return toLower(path.substr(dot + 1));
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::optional<TransferMethod> methodArg(const std::string& m) {
    std::string s = toLower(m);
    if (s.empty() || s == "webdav") return TransferMethod::WebDav;
    if (s == "http" || s == "crosspoint-http") return TransferMethod::CrossPointHttp;
    if (s == "ws" || s == "websocket" || s == "crosspoint-ws") return TransferMethod::CrossPointWs;
    if (s == "folder") return TransferMethod::Folder;
    if (s == "ble" || s == "bluetooth") return TransferMethod::Ble;
    return std::nullopt;
}

// Fills the Bluetooth fields of p from --ble (address or name), or scans
// and takes the only reader. Returns false after printing the problem.
bool resolveBleTarget(const Args& a, DeviceProfile& p) {
    std::string target = a.count("ble") ? a.at("ble") : "";
    if (target == "1") target.clear();   // "--ble" with no value
    if (!target.empty()) {
        if (auto addr = net::parseBleAddress(target)) p.bleAddress = *addr;
        else p.bleName = target;   // the transport scans for the name
        return true;
    }
    std::printf("Scanning 4 s for Bluetooth readers...\n");
    std::fflush(stdout);
    std::vector<net::BleDiscovered> list;
    try {
        list = net::discoverBle(4000);
    } catch (const TransferError& e) {
        std::fprintf(stderr, "Error: %s\n", e.message.c_str());
        return false;
    }
    if (list.size() == 1) {
        p.bleAddress = list[0].address;
        p.bleName = list[0].name;
        return true;
    }
    if (list.empty()) {
        std::fprintf(stderr, "Error: no Bluetooth reader found. %s.\n", net::kBleReaderHint);
        return false;
    }
    std::fprintf(stderr, "Error: %d Bluetooth readers found. Choose one with --ble <address or name>:\n",
                 static_cast<int>(list.size()));
    for (auto& d : list)
        std::fprintf(stderr, "  %s  %s  %d dBm\n", net::formatBleAddress(d.address).c_str(), d.name.c_str(), d.rssi);
    return false;
}

int cmdSend(const Args& a) {
    std::string file = a.at("send");
    auto m = methodArg(a.count("method") ? a.at("method") : "");
    if (!m) {
        std::fprintf(stderr, "Error: unknown method \"%s\".\n", a.at("method").c_str());
        return 1;
    }
    auto host = a.find("host");
    if (host == a.end() && *m != TransferMethod::Ble) {
        std::fprintf(stderr, "Error: --send needs --host <ip>.\n");
        return 1;
    }
    bool ok = false;
    OutputFile f;
    f.bytes = readFile(file, &ok);
    if (!ok) {
        std::fprintf(stderr, "Error: cannot read %s\n", file.c_str());
        return 1;
    }
    f.name = baseName(file);
    f.format = outputFormatFrom(extOf(file)).value_or(OutputFormat::Epub);
    f.mime = mimeFor(f.format);

    DeviceProfile p = net::defaultProfile(FirmwareType::CrossPoint);
    p.name = "CLI";
    p.host = host == a.end() ? std::string() : host->second;
    p.method = *m;
    if (a.count("port")) p.port = std::atoi(a.at("port").c_str());
    else if (*m == TransferMethod::CrossPointWs) p.port = 81;
    else p.port = 80;
    if (a.count("folder")) p.targetFolder = a.at("folder");
    if (a.count("user")) p.username = a.at("user");
    if (a.count("password")) p.password = a.at("password");

    if (p.method == TransferMethod::Ble) {
        if (!net::bleSupported()) {
            std::fprintf(stderr, "Error: Bluetooth transfer needs the Windows build.\n");
            return 1;
        }
        // Refuse a file over 16 MB before any scan (the transport checks again).
        if (f.bytes.size() > 16ull * 1024 * 1024) {
            std::fprintf(stderr, "Error: the file is %s. Bluetooth transfer allows at most 16 MB.\n",
                         humanBytes(f.bytes.size()).c_str());
            return 1;
        }
        if (!resolveBleTarget(a, p)) return 1;
    }
    std::string where = deviceWhere(p);
    std::printf("Sending %s (%s) to %s via %s...\n", f.name.c_str(), humanBytes(f.bytes.size()).c_str(),
                where.c_str(), toString(p.method).c_str());
    std::fflush(stdout);
    TransferState last = TransferState::Idle;
    int lastPct = -10;
    ProgressFn onProgress = [&](const TransferProgress& tp) {
        int pct = tp.fraction >= 0 ? static_cast<int>(tp.fraction * 100) : -1;
        if (tp.state != last || pct >= lastPct + 10) {
            std::printf("  %-10s %s%s\n", stateLabel(tp.state).c_str(),
                        pct >= 0 ? (std::to_string(pct) + "% ").c_str() : "", tp.message.c_str());
            std::fflush(stdout);
            last = tp.state;
            lastPct = pct;
        }
    };
    TransferResult r = net::transport(p.method).send(p, f, onProgress, nullptr);
    if (r.ok) {
        std::printf("OK: %s (%s)\n", r.remotePath.c_str(), humanBytes(r.bytes).c_str());
        return 0;
    }
    if (r.error) {
        net::Explanation ex = net::explain(*r.error, p.method);
        std::fprintf(stderr, "Failed (%s): %s\n", net::toString(r.error->code).c_str(), ex.title.c_str());
        for (auto& s : ex.reasons) std::fprintf(stderr, "  - %s\n", s.c_str());
        for (auto& s : ex.fallbacks) std::fprintf(stderr, "  > %s\n", s.c_str());
    } else {
        std::fprintf(stderr, "Failed: unknown error.\n");
    }
    return 1;
}

int cmdConvert(const Args& a) {
    std::string src = a.at("convert");
    auto outIt = a.find("out");
    if (outIt == a.end()) {
        std::fprintf(stderr, "Error: --convert needs --out <file>.\n");
        return 1;
    }
    std::string out = outIt->second;
    AppSettings s = net::loadSettings();
    RawInput in;
    bool ok = false;
    std::string bytes = readFile(src, &ok);
    if (ok) {
        in = rawFromFile(src, bytes);
        std::printf("Read %s (%s).\n", src.c_str(), humanBytes(bytes.size()).c_str());
    } else {
        auto url = extract::normalizeUrl(src);
        if (!url) {
            std::fprintf(stderr, "Error: %s is not a file or an http(s) URL.\n", src.c_str());
            return 1;
        }
        bool wiki = extract::isWikipediaUrl(*url);
        std::printf("Fetching %s%s...\n", url->c_str(), wiki ? " (Wikipedia API)" : "");
        std::fflush(stdout);
        extract::FetchResult r = wiki ? extract::fetchWikipedia(*url) : extract::fetchUrl(*url);
        if (!r.ok) {
            std::fprintf(stderr, "Fetch failed: %s (HTTP %d)\n", r.error.c_str(), r.status);
            return 1;
        }
        in.kind = wiki ? InputKind::Wikipedia : InputKind::Url;
        in.content = r.body;
        in.sourceUrl = r.finalUrl.empty() ? *url : r.finalUrl;
    }
    Article art = extract::extractArticle(in, s.extract);
    if (art.id.empty()) art.id = newUuid();
    if (art.capturedAt.empty()) art.capturedAt = nowIso();
    std::printf("Title: %s\nWords: %d, headings: %d, images: %d\n", art.title.c_str(), art.wordCount,
                static_cast<int>(art.toc.size()), static_cast<int>(art.images.size()));
    OutputFormat fmt = outputFormatFrom(extOf(out)).value_or(OutputFormat::Epub);
    BuildOptions o;
    o.style = s.style;
    o.includeImages = s.includeImages;
    o.includeCover = s.includeCover;
    o.includeSourceNote = s.includeSourceNote;
    ImageMap imgs;
    if (o.includeImages && !art.images.empty() && (fmt == OutputFormat::Epub || fmt == OutputFormat::Zip)) {
        imgs = extract::fetchImages(art);
        std::printf("Images loaded: %d of %d\n", static_cast<int>(imgs.size()), static_cast<int>(art.images.size()));
    }
    OutputFile f = buildFile(art, fmt, o, imgs);
    if (!writeFile(out, f.bytes)) {
        std::fprintf(stderr, "Error: cannot write %s\n", out.c_str());
        return 1;
    }
    std::printf("Wrote %s (%s, about %d pages).\n", out.c_str(), humanBytes(f.bytes.size()).c_str(),
                f.estimatedPages);
    return 0;
}

// Print one progress line when the text changes.
PackProgressFn printer(const char* label) {
    auto last = std::make_shared<std::string>();
    return [label, last](const PackProgress& p) {
        std::string s = std::string("  ") + label + " " + std::to_string(p.done) + "/" + std::to_string(p.total);
        if (!p.currentTitle.empty()) s += " " + p.currentTitle;
        if (!p.message.empty()) s += " (" + p.message + ")";
        if (s == *last) return;
        *last = s;
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
    };
}

// --pack: scan, auto-select, fetch, images, EPUB. Same steps as the wizard.
int cmdPack(const Args& a) {
    auto outIt = a.find("out");
    if (outIt == a.end()) {
        std::fprintf(stderr, "Error: --pack needs --out <file.epub>.\n");
        return 1;
    }
    AppSettings s = net::loadSettings();
    PackLimits limits = s.packLimits;
    if (a.count("depth")) limits.depth = std::clamp(std::atoi(a.at("depth").c_str()), 0, 2);
    if (a.count("max")) limits.maxArticles = std::clamp(std::atoi(a.at("max").c_str()), 1, 500);
    std::string lang = a.count("lang") ? a.at("lang") : "en";
    auto root = spider::normalizeWikipediaTitle(a.at("pack"), lang);
    if (!root) {
        std::fprintf(stderr, "Error: \"%s\" is not a Wikipedia article URL or title.\n", a.at("pack").c_str());
        return 1;
    }
    std::printf("Scanning %s (depth %d, max %d articles)...\n", root->url.c_str(), limits.depth, limits.maxArticles);
    std::fflush(stdout);
    auto cands = spider::scanWikipediaLinks(*root, limits, printer("scanned"));
    if (cands.empty()) {
        std::fprintf(stderr, "Error: the scan found no page.\n");
        return 1;
    }
    Pack pack = spider::newPack(PackSourceKind::Wikipedia, cands.front().key, cands.front().url,
                                cands.front().title + " pack", limits);
    spider::fillPagesFromCandidates(pack, cands);
    int sel = 0;
    for (auto& c : cands) sel += c.selected ? 1 : 0;
    std::printf("Found %d pages, %d selected.\n", static_cast<int>(cands.size()), sel);
    pack = spider::fetchPackPages(pack, packExtractOptions(s, limits), printer("page"));
    int done = 0, failed = 0;
    for (auto& p : pack.pages) {
        done += p.state == PackPageState::Done ? 1 : 0;
        if (p.state == PackPageState::Failed) {
            ++failed;
            std::printf("  failed: %s: %s\n", p.candidate.title.c_str(), p.error.c_str());
        }
    }
    if (!done) {
        std::fprintf(stderr, "Error: no page could be fetched.\n");
        return 1;
    }
    BuildOptions o;
    o.style = s.style;
    o.includeImages = s.includeImages;
    o.includeCover = s.includeCover;
    o.includeSourceNote = s.includeSourceNote;
    ImageMap imgs;
    if (o.includeImages && limits.maxImagesPerArticle > 0) {
        for (auto& p : pack.pages) {
            if (p.state != PackPageState::Done || !p.article || p.article->images.empty()) continue;
            Article art = *p.article;
            if (art.images.size() > static_cast<size_t>(limits.maxImagesPerArticle))
                art.images.resize(static_cast<size_t>(limits.maxImagesPerArticle));
            for (auto& kv : extract::fetchImages(art)) imgs.insert(kv);
        }
        std::printf("Images loaded: %d\n", static_cast<int>(imgs.size()));
    }
    spider::Rewritten rw = spider::rewriteLinks(pack.pages, pack.kind);
    pack.pages = std::move(rw.pages);
    pack.externalLinks = std::move(rw.external);
    OutputFile f = output::buildPackEpub(pack, o, imgs, printer("building"));
    if (!writeFile(outIt->second, f.bytes)) {
        std::fprintf(stderr, "Error: cannot write %s\n", outIt->second.c_str());
        return 1;
    }
    std::printf("Wrote %s (%s): %d articles, %d failed, %d images, about %d pages.\n", outIt->second.c_str(),
                humanBytes(f.bytes.size()).c_str(), done, failed, static_cast<int>(imgs.size()), f.estimatedPages);
    return 0;
}

} // namespace

bool isCliArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--", 0) == 0 || a == "-h" || a == "/?") return true;
    }
    return false;
}

// Lists the Bluetooth readers. Returns the number found (0 on error).
int discoverBlePrint() {
    if (!net::bleSupported()) {
        std::printf("Bluetooth: not available in this build (Windows only).\n");
        return 0;
    }
    std::printf("Discovering: Bluetooth LE scan for 4 s (CrossPoint file service)...\n");
    std::fflush(stdout);
    std::vector<net::BleDiscovered> list;
    try {
        list = net::discoverBle(4000);
    } catch (const TransferError& e) {
        std::printf("Bluetooth: %s\n", e.message.c_str());
        return 0;
    }
    if (list.empty()) std::printf("No Bluetooth reader found. %s.\n", net::kBleReaderHint);
    for (const auto& d : list) {
        std::string addr = net::formatBleAddress(d.address);
        std::printf("ble %s  %s  %d dBm  (send: --method ble --ble %s)\n", addr.c_str(), d.name.c_str(), d.rssi,
                    addr.c_str());
    }
    return static_cast<int>(list.size());
}

int cmdDiscover(const Args& a) {
    if (a.count("discover-ble") && !a.count("discover")) return discoverBlePrint() > 0 ? 0 : 1;
    int timeout = 2000;
    auto it = a.find("timeout");
    if (it != a.end() && !it->second.empty()) timeout = std::atoi(it->second.c_str());
    std::printf("Discovering: UDP \"hello\" to port 8134 (broadcast + unicast sweep), then an HTTP probe of port 80...\n");
    std::fflush(stdout);
    std::vector<net::Discovered> list = net::discoverCrossPoint(timeout);
    if (list.empty())
        std::printf("No CrossPoint reader found on the LAN. Put the reader on the File Transfer screen, and check that this PC is on the same network.\n");
    for (const auto& d : list)
        std::printf("%s  ws-port %d  host %s  via %s\n", d.ip.c_str(), d.wsPort, d.hostname.c_str(), d.raw.c_str());
    int ble = discoverBlePrint();
    return list.empty() && ble == 0 ? 1 : 0;
}

int runCli(int argc, char** argv) {
    try {
        Args a = parseArgs(argc, argv);
        if (a.count("help") || a.count("h") || (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "/?"))) {
            std::printf("%s", kUsage);
            return 0;
        }
        if (a.count("send")) return cmdSend(a);
        if (a.count("convert")) return cmdConvert(a);
        if (a.count("pack")) return cmdPack(a);
        if (a.count("discover") || a.count("discover-ble")) return cmdDiscover(a);
        std::fprintf(stderr, "Unknown arguments.\n\n%s", kUsage);
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
}

} // namespace ks::app
