// Background jobs: fetch, extract, images, export, send, test, discover.
// Each job copies its inputs. It never reads App from the worker thread.
// It posts a UiTask with the result.
#include "app/app.h"

#include <cctype>
#include <cstdio>

namespace ks::app {

namespace {

// Percent-encode a Wikipedia title. Spaces become underscores.
std::string encodeTitle(const std::string& t) {
    std::string out;
    for (unsigned char c : trim(t)) {
        if (c == ' ') out += '_';
        else if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == ',')
            out += static_cast<char>(c);
        else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// Extract on the worker thread, then post the article to the UI.
void extractAndPost(const JobCtx& ctx, const RawInput& in, const ExtractOptions& opts) {
    if (ctx.cancelled()) return;
    ctx.log("Extract: " + toString(in.kind) + (in.sourceUrl ? " from " + *in.sourceUrl : std::string()));
    Article a = extract::extractArticle(in, opts);
    if (a.id.empty()) a.id = newUuid();
    if (a.capturedAt.empty()) a.capturedAt = nowIso();
    std::vector<PreviewLine> lines = buildPreviewLines(a);
    if (ctx.cancelled()) return;
    ctx.log("Loaded: \"" + a.title + "\", " + std::to_string(a.wordCount) + " words, " +
            std::to_string(a.images.size()) + " images.");
    ctx.post([in, a = std::move(a), lines = std::move(lines)](App& app) mutable {
        app.lastInput = in;
        app.sourceError.clear();
        app.sourceFallbacks.clear();
        app.sourceStatus = "Loaded: " + a.title;
        app.toast(ToastKind::Info, "loaded: " + (a.title.empty() ? std::string("(untitled)") : a.title));
        app.setArticle(std::move(a), std::move(lines));
    });
}

void postSourceError(const JobCtx& ctx, const std::string& msg, std::vector<std::string> fallbacks) {
    ctx.log("Source error: " + msg);
    ctx.post([msg, fallbacks = std::move(fallbacks)](App& app) {
        app.sourceStatus.clear();
        app.sourceError = msg;
        app.sourceFallbacks = fallbacks;
        app.toast(ToastKind::Error, msg);
    });
}

std::vector<std::string> fetchFallbacks() {
    return {"Open the page in your browser, select all, copy, then press Paste here.",
            "Save the page as HTML and drop the file on this window.",
            "Use the bookmarklet in the web app, then paste its payload as HTML."};
}

// Fetch one URL (Wikipedia through the API) and extract it.
void fetchBody(const JobCtx& ctx, const std::string& url, const ExtractOptions& opts) {
    bool wiki = extract::isWikipediaUrl(url);
    ctx.log(std::string(wiki ? "Fetch Wikipedia: " : "Fetch: ") + url);
    extract::FetchResult r = wiki ? extract::fetchWikipedia(url) : extract::fetchUrl(url, &ctx.cancelFn);
    if (ctx.cancelled()) {
        ctx.log("Fetch cancelled.");
        ctx.post([](App& app) { app.sourceStatus = "Cancelled."; });
        return;
    }
    if (!r.ok) {
        std::string msg = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        postSourceError(ctx, "Fetch failed: " + msg, fetchFallbacks());
        return;
    }
    ctx.log("Fetched " + humanBytes(r.body.size()) + " (HTTP " + std::to_string(r.status) + ").");
    RawInput in;
    in.kind = wiki ? InputKind::Wikipedia : InputKind::Url;
    in.content = std::move(r.body);
    in.sourceUrl = r.finalUrl.empty() ? url : r.finalUrl;
    extractAndPost(ctx, in, opts);
}

} // namespace

TransferRecord makeRecord(const std::string& articleId, const DeviceProfile& p, const OutputFile& f,
                          const TransferResult& res) {
    TransferRecord r;
    r.id = newUuid();
    r.articleId = articleId;
    r.deviceId = p.id;
    r.deviceName = p.name;
    r.method = p.method;
    r.format = f.format;
    r.fileName = f.name;
    r.bytes = res.ok ? res.bytes : f.bytes.size();
    r.ok = res.ok;
    if (res.error) r.error = res.error->message;
    r.at = nowIso();
    return r;
}

// Send one file with a progress callback that posts to the UI.
TransferResult sendWithProgress(const JobCtx& ctx, const DeviceProfile& p, const OutputFile& f) {
    auto ctl = ctx.ctl;
    auto sh = ctx.sh;
    ProgressFn onProgress = [ctl, sh](const TransferProgress& tp) {
        if (tp.fraction >= 0) ctl->progress = static_cast<float>(tp.fraction);
        sh->post([tp](App& app) { app.transfer = tp; });
    };
    ctx.log("Send " + f.name + " (" + humanBytes(f.bytes.size()) + ") to " + p.name + " via " +
            toString(p.method) + " at " + deviceWhere(p));
    TransferResult res = net::transport(p.method).send(p, f, onProgress, &ctl->cancelPlain);
    if (res.ok) {
        ctx.log("Sent: " + res.remotePath + " (" + humanBytes(res.bytes) + ").");
        try {
            if (!p.id.empty()) net::markConnected(p.id);
        } catch (const std::exception& e) {
            ctx.log(std::string("markConnected: ") + e.what());
        }
    } else {
        ctx.log("Send failed: " + (res.error ? net::toString(res.error->code) + ": " + res.error->message
                                             : std::string("unknown error")));
    }
    return res;
}

void postSendResult(const JobCtx& ctx, const TransferResult& res, const std::string& name,
                    const std::string& device, TransferMethod method) {
    ctx.post([res, name, device, method](App& app) {
        if (res.ok) {
            app.transfer.state = TransferState::Complete;
            app.transfer.fraction = 1.0;
            app.transfer.message = "Saved as " + res.remotePath;
            app.lastError.reset();
            app.lastExplain.clear();
            app.sendStatus = "Sent " + name + " (" + humanBytes(res.bytes) + ").";
            app.reloadProfiles();   // the send set lastConnectedAt
            app.toast(ToastKind::Info, "sent " + name + " to " + device + " (" + humanBytes(res.bytes) + ")");
        } else {
            app.transfer.state = TransferState::Failed;
            app.lastError = res.error ? *res.error : TransferError{TransferErrorCode::Unknown, "The send failed."};
            app.transfer.message = app.lastError->message;
            app.sendStatus.clear();
            // The menu bar has no room for the explain block. The toast gets
            // the first reason. The log and the pill tooltip get all of it.
            net::Explanation ex = net::explain(*app.lastError, method);
            std::string text = ex.title + "\n" + app.lastError->message;
            for (auto& r : ex.reasons) text += "\n- " + r;
            if (!ex.fallbacks.empty()) text += "\nWhat you can do:";
            for (auto& f : ex.fallbacks) text += "\n- " + f;
            app.lastExplain = text;
            app.addLog("Send failed: " + ex.title);
            for (auto& r : ex.reasons) app.addLog("  reason: " + r);
            for (auto& f : ex.fallbacks) app.addLog("  try: " + f);
            app.toast(ToastKind::Error, "send failed: " + (ex.reasons.empty() ? app.lastError->message : ex.reasons[0]));
        }
        if (app.showLibrary) jobRefreshLibrary(app);
    });
}

void jobLoadUrl(App& app, const std::string& input) {
    auto url = extract::normalizeUrl(input);
    if (!url) {
        app.sourceError = "This is not a valid http or https URL.";
        app.sourceFallbacks.clear();
        return;
    }
    app.sourceError.clear();
    app.sourceStatus = "Fetching...";
    ExtractOptions opts = app.settings.extract;
    std::string u = *url;
    app.startJob(JobKind::Source, "Fetch " + u, [u, opts](JobCtx& ctx) { fetchBody(ctx, u, opts); });
}

void jobWikipedia(App& app, const std::string& titleOrUrl, const std::string& lang) {
    std::string t = trim(titleOrUrl);
    if (t.empty()) {
        app.sourceError = "Type an article title or a Wikipedia URL first.";
        return;
    }
    std::string url;
    auto norm = extract::normalizeUrl(t);
    if (norm && extract::isWikipediaUrl(*norm)) url = *norm;
    else {
        std::string l = trim(lang).empty() ? "en" : toLower(trim(lang));
        url = "https://" + l + ".wikipedia.org/wiki/" + encodeTitle(t);
    }
    jobLoadUrl(app, url);
}

void jobExtract(App& app, RawInput in) {
    if (trim(in.content).empty()) {
        app.sourceError = "The box is empty.";
        return;
    }
    app.sourceError.clear();
    app.sourceStatus = "Converting...";
    ExtractOptions opts = app.settings.extract;
    app.startJob(JobKind::Source, "Convert " + toString(in.kind), [in = std::move(in), opts](JobCtx& ctx) {
        extractAndPost(ctx, in, opts);
    });
}

void jobLoadFile(App& app, const std::string& path) {
    if (trim(path).empty()) {
        app.sourceError = "Choose a file first.";
        return;
    }
    app.sourceError.clear();
    app.sourceStatus = "Reading " + path;
    ExtractOptions opts = app.settings.extract;
    app.startJob(JobKind::Source, "Read file", [path, opts](JobCtx& ctx) {
        bool ok = false;
        std::string bytes = readFile(path, &ok);
        if (!ok) {
            postSourceError(ctx, "Cannot read the file: " + path, {});
            return;
        }
        ctx.log("Read " + path + " (" + humanBytes(bytes.size()) + ").");
        extractAndPost(ctx, rawFromFile(path, bytes), opts);
    });
}

void jobImages(App& app) {
    if (!app.article) return;
    Article a = *app.article;
    app.startJob(JobKind::Images, "Fetch " + std::to_string(a.images.size()) + " images", [a](JobCtx& ctx) {
        ImageMap m = extract::fetchImages(a, &ctx.cancelFn);
        ctx.log("Images: " + std::to_string(m.size()) + " of " + std::to_string(a.images.size()) + " loaded.");
        if (ctx.cancelled()) return;
        std::string id = a.id;
        ctx.post([id, m = std::move(m)](App& app) mutable {
            if (!app.article || app.article->id != id) return;
            app.images = std::move(m);
            app.imagesFetched = true;
        });
    });
}

void jobExport(App& app, const std::string& path) {
    if (!app.article) return;
    Article a = *app.article;
    OutputFormat fmt = app.format;
    BuildOptions o = app.buildOptions();
    FilenameRules rules = app.settings.filename;
    ImageMap imgs = app.images;
    bool needImages = o.includeImages && !app.imagesFetched && !a.images.empty();
    bool cache = app.settings.cacheImages;
    app.exportStatus = "Building...";
    app.startJob(JobKind::Export, "Export " + formatLabel(fmt), [=](JobCtx& ctx) mutable {
        if (needImages) imgs = extract::fetchImages(a, &ctx.cancelFn);
        if (ctx.cancelled()) return;
        OutputFile f = buildFile(a, fmt, o, imgs);
        f.name = output::makeFilename(a, fmt, rules);
        bool ok = writeFile(path, f.bytes);
        ctx.log(ok ? "Exported " + path + " (" + humanBytes(f.bytes.size()) + ")." : "Cannot write " + path);
        TransferRecord r;
        r.id = newUuid();
        r.articleId = a.id;
        r.deviceName = "Local export";
        r.method = TransferMethod::Folder;
        r.format = fmt;
        r.fileName = path;
        r.bytes = f.bytes.size();
        r.ok = ok;
        if (!ok) r.error = "Cannot write the file.";
        r.at = nowIso();
        saveToLibrary(ctx, a, f, cache ? &imgs : nullptr, r);
        ctx.post([ok, path, n = f.bytes.size()](App& app) {
            app.exportStatus = ok ? "Saved " + path + " (" + humanBytes(n) + ")." : "Cannot write " + path;
            app.toast(ok ? ToastKind::Info : ToastKind::Error, app.exportStatus);
            if (app.showLibrary) jobRefreshLibrary(app);
        });
    });
}

void jobSend(App& app) {
    auto prof = app.currentProfile();
    if (!prof) {
        app.sendStatus = "Add or choose a device first.";
        return;
    }
    if (!app.article) {
        app.sendStatus = "Load an article first.";
        return;
    }
    Article a = *app.article;
    OutputFormat fmt = app.format;
    BuildOptions o = app.buildOptions();
    FilenameRules rules = app.settings.filename;
    ImageMap imgs = app.images;
    bool needImages = o.includeImages && !app.imagesFetched && !a.images.empty();
    bool cache = app.settings.cacheImages;
    DeviceProfile p = *prof;
    app.lastError.reset();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Building file"};
    app.startJob(JobKind::Send, "Send to " + p.name, [=](JobCtx& ctx) mutable {
        if (needImages) imgs = extract::fetchImages(a, &ctx.cancelFn);
        OutputFile f = buildFile(a, fmt, o, imgs);
        f.name = output::makeFilename(a, fmt, rules);
        TransferResult res = sendWithProgress(ctx, p, f);
        saveToLibrary(ctx, a, f, cache ? &imgs : nullptr, makeRecord(a.id, p, f, res));
        postSendResult(ctx, res, f.name, p.name, p.method);
    });
}

void jobSendFile(App& app, OutputFile file) {
    auto prof = app.currentProfile();
    if (!prof) {
        app.sendStatus = "Add or choose a device first.";
        return;
    }
    jobSendFileTo(app, *prof, std::move(file));
}

void jobSendFileTo(App& app, const DeviceProfile& p, OutputFile file) {
    app.lastError.reset();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Connecting"};
    app.startJob(JobKind::Send, "Send " + file.name, [p, file](JobCtx& ctx) {
        TransferResult res = sendWithProgress(ctx, p, file);
        postSendResult(ctx, res, file.name, p.name, p.method);
    });
}

void jobFallbackFolder(App& app, const std::string& folder) {
    if (!app.article) return;
    auto prof = app.currentProfile();
    DeviceProfile p = prof ? *prof : net::defaultProfile(FirmwareType::CrossPoint);
    p.name = "USB folder";
    p.host = folder;
    p.method = TransferMethod::Folder;
    Article a = *app.article;
    OutputFormat fmt = app.format;
    BuildOptions o = app.buildOptions();
    FilenameRules rules = app.settings.filename;
    ImageMap imgs = app.images;
    app.lastError.reset();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Writing to folder"};
    app.startJob(JobKind::Send, "Write to " + folder, [=](JobCtx& ctx) {
        OutputFile f = buildFile(a, fmt, o, imgs);
        f.name = output::makeFilename(a, fmt, rules);
        TransferResult res = sendWithProgress(ctx, p, f);
        saveToLibrary(ctx, a, f, nullptr, makeRecord(a.id, p, f, res));
        postSendResult(ctx, res, f.name, p.name, p.method);
    });
}

std::string testKey(const DeviceProfile& p) {
    if (p.method == TransferMethod::Ble)
        return net::formatBleAddress(p.bleAddress) + "/" + p.bleName + ":" + toString(p.method);
    return trim(p.host) + ":" + std::to_string(p.port) + ":" + toString(p.method);
}

void jobTest(App& app, const DeviceProfile& p, TestTarget target) {
    switch (target) {
        case TestTarget::Form:
            app.dev.test.reset();
            app.dev.testedKey = testKey(p);
            break;
        case TestTarget::Panel: app.testResult.reset(); break;
    }
    app.startJob(JobKind::Test, "Test " + deviceWhere(p), [p, target](JobCtx& ctx) {
        ctx.log("Test connection: " + toString(p.method) + " " + deviceWhere(p));
        ConnectionTestResult r = net::transport(p.method).testConnection(p);
        ctx.log(r.ok ? "Test OK (" + std::to_string(r.latencyMs) + " ms)."
                     : "Test failed: " + (r.error ? r.error->message : std::string("unknown error")));
        if (r.ok && !p.id.empty()) {
            try {
                net::markConnected(p.id);
            } catch (const std::exception&) {
            }
        }
        ctx.post([r, target](App& app) {
            switch (target) {
                case TestTarget::Form: app.dev.test = r; break;
                case TestTarget::Panel:
                    app.testResult = r;
                    break;
            }
            if (r.ok) app.reloadProfiles();   // the test set lastConnectedAt
            if (r.ok) app.toast(ToastKind::Info, "connected (" + std::to_string(r.latencyMs) + " ms)");
            else app.toast(ToastKind::Error, "test failed: " + (r.error ? r.error->message : std::string("unknown")));
        });
    });
}

void jobConfirmDevice(App& app, const DeviceProfile& draft) {
    DeviceProfile p = draft;
    app.lastError.reset();
    app.sendStatus.clear();
    app.dev.problems.clear();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Connecting"};
    app.startJob(JobKind::Send, "Confirm " + p.name, [p](JobCtx& ctx) {
        OutputFile file = output::testEpub();
        TransferResult res = sendWithProgress(ctx, p, file);
        postSendResult(ctx, res, file.name, p.name, p.method);
        if (!res.ok) return;
        // The test file arrived. Save the profile, then set lastConnectedAt.
        DeviceProfile saved;
        std::string problem;
        try {
            saved = net::saveProfile(p);
            net::markConnected(saved.id);
        } catch (const std::exception& e) {
            problem = e.what();
        }
        ctx.post([saved, problem](App& app) {
            if (!problem.empty()) {
                app.dev.problems = {"The file arrived, but the device was not saved: " + problem};
                return;
            }
            app.addLog("Device confirmed: " + saved.name + " (" + deviceWhere(saved) + ").");
            app.reloadProfiles();
            app.profileId = saved.id;
            app.method = saved.method;
            app.settings.defaultDeviceId = saved.id;
            app.markSettingsChanged();
            app.dev = DeviceFlow{};   // back to the list of the devices window
            app.dev.autoDiscovered = true;
            app.testResult.reset();
            app.toast(ToastKind::Info, "device confirmed: " + saved.name);
        });
    });
}

void jobDiscover(App& app) {
    app.discovered.clear();
    app.discoverDone = false;
    int timeoutMs = app.prefs.discovery_timeout_ms;
    app.startJob(JobKind::Discover, "Discover CrossPoint devices", [timeoutMs](JobCtx& ctx) {
        ctx.log("Discover: UDP \"hello\" to port 8134 (broadcast + unicast sweep), then an HTTP probe of port 80.");
        auto list = net::discoverCrossPoint(timeoutMs);
        ctx.log("Discover: " + std::to_string(list.size()) + " device(s) found.");
        for (auto& d : list) ctx.log("  " + d.ip + " " + d.hostname + " ws:" + std::to_string(d.wsPort));
        ctx.post([list](App& app) {
            app.discovered = list;
            app.discoverDone = true;
            if (list.empty()) app.toast(ToastKind::Warn, "no device replied to discovery");
            else app.toast(ToastKind::Info, std::to_string(list.size()) + " device(s) found");
        });
    });
}

void jobDiscoverBle(App& app) {
    app.bleFound.clear();
    app.bleScanDone = false;
    app.bleScanError.clear();
    app.dev.bleSel = -1;
    app.startJob(JobKind::BleScan, "Scan for Bluetooth readers", [](JobCtx& ctx) {
        ctx.log("Bluetooth: scanning 4 s for the CrossPoint file service.");
        std::vector<net::BleDiscovered> list;
        std::string err;
        try {
            list = net::discoverBle(4000, &ctx.ctl->cancelPlain);
        } catch (const TransferError& e) {
            err = e.message;
        } catch (const std::exception& e) {
            err = e.what();
        }
        if (!err.empty()) ctx.log("Bluetooth scan failed: " + err);
        else ctx.log("Bluetooth: " + std::to_string(list.size()) + " reader(s) found.");
        for (auto& d : list)
            ctx.log("  " + d.name + " " + net::formatBleAddress(d.address) + " " + std::to_string(d.rssi) + " dBm");
        ctx.post([list, err](App& app) {
            app.bleFound = list;
            app.bleScanDone = true;
            app.bleScanError = err;
            if (!err.empty()) app.toast(ToastKind::Error, "bluetooth: " + err);
            else if (list.empty()) app.toast(ToastKind::Warn, "no Bluetooth reader found");
            else app.toast(ToastKind::Info, std::to_string(list.size()) + " Bluetooth reader(s) found");
        });
    });
}

} // namespace ks::app
