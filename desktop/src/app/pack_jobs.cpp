// Spider pack jobs: scan, build (pages, images, EPUB), export, send, and
// the library actions for stored packs. Each job copies its inputs. It
// never reads App on the worker thread. It posts a UiTask with the result.
#include "app/app.h"
#include "spider/spider.h"

#include <algorithm>

namespace ks::app {

// ---------------------------------------------------------- small helpers

bool packShown(const PackPage& p) {
    return p.candidate.selected || p.state == PackPageState::Done || p.state == PackPageState::Failed;
}

PackStats packStats(const Pack& p, const StyleSettings& style) {
    PackStats s;
    for (const auto& pg : p.pages) {
        if (packShown(pg)) ++s.shown;
        if (pg.state == PackPageState::Failed) ++s.failed;
        if (pg.state != PackPageState::Done || !pg.article) continue;
        ++s.articles;
        s.words += pg.article->wordCount;
        s.pages += output::estimateReading(*pg.article, style).pages;
        s.images += std::min(static_cast<int>(pg.article->images.size()), p.limits.maxImagesPerArticle);
    }
    return s;
}

ExtractOptions packExtractOptions(const AppSettings& s, const PackLimits& l) {
    ExtractOptions o = s.extract;
    o.keepLinks = true;   // the links between the pages work on the reader
    o.keepImages = s.includeImages;
    o.keepInfobox = l.includeInfobox;
    o.keepReferences = l.includeReferences;
    return o;
}

std::string packFinalName(const App& app) {
    std::string base = app.pk.fileName.empty() && app.pk.file ? app.pk.file->name : app.pk.fileName;
    if (base.size() > 5 && toLower(base.substr(base.size() - 5)) == ".epub") base.resize(base.size() - 5);
    std::string safe = output::sanitizeFilename(base, app.settings.filename);
    return (safe.empty() ? std::string("pack") : safe) + ".epub";
}

void packRestart(App& app) {
    app.pk.step = 0;
    app.pk.hasRoot = false;
    app.pk.limits = app.settings.packLimits;
    app.pk.limitsSet = true;
    app.pk.scanLine.clear();
    app.pk.buildLine.clear();
    app.pk.errors.clear();
}

std::string packSetRoot(App& app) {
    PackUi& k = app.pk;
    if (k.webSource) {
        if (!app.article) return "Load an article first.";
        const Article& a = *app.article;
        std::string key = a.sourceUrl.empty() ? std::string() : spider::webKey(a.sourceUrl);
        if (key.empty()) key = a.sourceUrl.empty() ? a.id : a.sourceUrl;
        k.rootKind = PackSourceKind::Web;
        k.rootKey = key;
        k.rootTitle = a.title;
        k.rootUrl = a.sourceUrl;
        k.rootLang = a.language.empty() ? "en" : a.language;
        k.rootArticle = a;
    } else {
        std::string lang = trim(k.wikiLang).empty() ? "en" : toLower(trim(k.wikiLang));
        auto n = spider::normalizeWikipediaTitle(k.wikiInput, lang);
        if (!n) return "Type a Wikipedia article URL or title.";
        k.rootKind = PackSourceKind::Wikipedia;
        k.rootKey = n->title;
        k.rootTitle = n->title;
        k.rootUrl = n->url;
        k.rootLang = n->lang;
        k.rootArticle.reset();
    }
    k.hasRoot = true;
    if (!k.limitsSet) {
        k.limits = app.settings.packLimits;
        k.limitsSet = true;
    }
    return {};
}

namespace {

std::string progressText(const std::string& label, const PackProgress& p) {
    std::string s = label + " " + std::to_string(p.done) + " / " + std::to_string(p.total);
    if (!p.currentTitle.empty()) s += " - " + p.currentTitle;
    if (!p.message.empty()) s += " (" + p.message + ")";
    return s;
}

// Fetch the images of each fetched page, at most maxImagesPerArticle each.
ImageMap fetchPackImages(const JobCtx& ctx, const Pack& pack) {
    ImageMap out;
    std::vector<const PackPage*> pages;
    for (const auto& p : pack.pages)
        if (p.state == PackPageState::Done && p.article && !p.article->images.empty()) pages.push_back(&p);
    const size_t max = static_cast<size_t>(std::max(0, pack.limits.maxImagesPerArticle));
    for (size_t i = 0; i < pages.size(); ++i) {
        if (ctx.cancelled()) break;
        Article a = *pages[i]->article;
        if (a.images.size() > max) a.images.resize(max);
        std::string line = "Images: page " + std::to_string(i) + " / " + std::to_string(pages.size()) + " - " + a.title;
        ctx.progress(0.6f + 0.3f * static_cast<float>(i) / static_cast<float>(pages.size()));
        ctx.post([line](App& app) { app.pk.buildLine = line; });
        for (auto& kv : extract::fetchImages(a, &ctx.cancelFn)) out.insert(kv);
    }
    return out;
}

// Rewrite the links and build the EPUB. The pack gets the rewritten pages.
OutputFile buildPack(const JobCtx& ctx, Pack& pack, const BuildOptions& o, const ImageMap& imgs) {
    spider::Rewritten rw = spider::rewriteLinks(pack.pages, pack.kind);
    pack.pages = std::move(rw.pages);
    pack.externalLinks = std::move(rw.external);
    PackProgressFn onProgress = [ctx](const PackProgress& p) {
        std::string line = progressText("Building", p);
        ctx.post([line](App& app) { app.pk.buildLine = line; });
    };
    return output::buildPackEpub(pack, o, o.includeImages ? imgs : ImageMap{}, onProgress);
}

// The pack file for an export or a send. Build again when the title or the
// author changed after the last build. Posts the new file to the UI.
struct PackOut {
    Pack pack;
    ImageMap images;
    std::optional<OutputFile> file;
    std::string builtTitle, builtAuthor;
    std::string name;
};

PackOut packOut(const App& app) {
    PackOut o;
    o.pack = *app.pk.pack;
    o.images = app.pk.images;
    o.file = app.pk.file;
    o.builtTitle = app.pk.builtTitle;
    o.builtAuthor = app.pk.builtAuthor;
    o.name = packFinalName(app);
    return o;
}

OutputFile currentFile(const JobCtx& ctx, PackOut& o, const BuildOptions& opts) {
    if (o.file && o.pack.title == o.builtTitle && o.pack.author == o.builtAuthor) {
        OutputFile f = *o.file;
        f.name = o.name;
        return f;
    }
    ctx.log("Pack: the title or the author changed. Building the EPUB again.");
    OutputFile f = buildPack(ctx, o.pack, opts, o.images);
    std::string id = o.pack.id, title = o.pack.title, author = o.pack.author;
    ctx.post([id, f, title, author](App& app) {
        if (!app.pk.pack || app.pk.pack->id != id) return;
        app.pk.file = f;
        app.pk.builtTitle = title;
        app.pk.builtAuthor = author;
    });
    f.name = o.name;
    return f;
}

TransferRecord localRecord(const std::string& id, const OutputFile& f, const std::string& path, bool ok) {
    TransferRecord r;
    r.id = newUuid();
    r.articleId = id;
    r.deviceName = "Local export";
    r.method = TransferMethod::Folder;
    r.format = OutputFormat::Epub;
    r.fileName = path;
    r.bytes = f.bytes.size();
    r.ok = ok;
    if (!ok) r.error = "Cannot write the file.";
    r.at = nowIso();
    return r;
}

void recordPack(const JobCtx& ctx, const std::string& id, const std::string& name, const std::optional<TransferRecord>& r) {
    try {
        net::updatePackRecord(id, name, r);
    } catch (const std::exception& e) {
        ctx.log(std::string("Pack library update failed: ") + e.what());
    }
}

// Load a stored pack and build its EPUB again from the stored pages and images.
struct Stored {
    Pack pack;
    ImageMap images;
    OutputFile file;
    std::string fileName;
};

std::optional<Stored> loadStored(const JobCtx& ctx, const std::string& id, const BuildOptions& o) {
    auto p = net::getPack(id);
    if (!p) {
        ctx.log("The pack is not in the library: " + id);
        return std::nullopt;
    }
    Stored s;
    s.pack = *p;
    s.images = net::loadPackImages(id);
    s.fileName = net::getPackRecord(id).fileName;
    s.file = buildPack(ctx, s.pack, o, s.images);
    if (!s.fileName.empty()) s.file.name = s.fileName;
    return s;
}

} // namespace

void jobPackScan(App& app) {
    PackUi& k = app.pk;
    if (!k.hasRoot) return;
    k.scanLine = "Scanning...";
    k.scanFailed = false;
    PackSourceKind kind = k.rootKind;
    std::string key = k.rootKey, title = k.rootTitle, url = k.rootUrl, lang = k.rootLang;
    std::optional<Article> art = k.rootArticle;
    PackLimits limits = k.limits;
    app.startJob(JobKind::Pack, "Scan " + title, [=](JobCtx& ctx) {
        std::vector<PackCandidate> cands;
        try {
            if (kind == PackSourceKind::Wikipedia) {
                spider::WikiRoot root{lang, title, url};
                PackProgressFn onProgress = [ctx](const PackProgress& p) {
                    std::string line = progressText("scanned", p);
                    if (p.total) ctx.progress(static_cast<float>(p.done) / static_cast<float>(p.total));
                    ctx.post([line](App& app) { app.pk.scanLine = line; });
                };
                cands = spider::scanWikipediaLinks(root, limits, onProgress, &ctx.ctl->cancelPlain);
            } else {
                PackCandidate rc;
                rc.key = key;
                rc.title = title;
                rc.url = url;
                rc.selected = true;
                cands.push_back(rc);
                if (limits.depth > 0 && art)
                    for (auto& c : spider::scanWebLinks(*art, url, limits))
                        if (c.key != key) cands.push_back(c);
            }
        } catch (const spider::CancelledError&) {
            ctx.post([](App& app) { app.pk.scanLine = "Scan cancelled."; });
            return;
        } catch (const std::exception& e) {
            std::string msg = e.what();
            ctx.log("Scan failed: " + msg);
            ctx.post([msg](App& app) {
                app.pk.scanLine = "Scan failed: " + msg;
                app.pk.scanFailed = true;
                app.toast(ToastKind::Error, "scan failed: " + msg);
            });
            return;
        }
        if (cands.empty()) {
            ctx.post([](App& app) {
                app.pk.scanLine = "Scan failed: no page found.";
                app.pk.scanFailed = true;
            });
            return;
        }
        const PackCandidate* rootCand = &cands.front();
        for (auto& c : cands)
            if (c.depth == 0) {
                rootCand = &c;
                break;
            }
        Pack pack = spider::newPack(kind, rootCand->key, rootCand->url, rootCand->title + " pack", limits);
        spider::fillPagesFromCandidates(pack, cands);
        if (kind == PackSourceKind::Web && art && !pack.pages.empty()) {
            pack.pages[0].state = PackPageState::Done;   // the root is loaded: fetchPackPages keeps it
            pack.pages[0].article = art;
        }
        int sel = 0;
        for (auto& c : cands) sel += c.selected ? 1 : 0;
        std::string line = "found " + std::to_string(cands.size()) + " pages (" + std::to_string(sel) + " selected).";
        ctx.log("Scan: " + line);
        ctx.post([pack = std::move(pack), line](App& app) mutable {
            app.pk.pack = std::move(pack);
            app.pk.file.reset();
            app.pk.images.clear();
            app.pk.fileName.clear();
            app.pk.previewKey.clear();
            app.pk.scanLine = line;
            app.pk.step = 2;
            app.pk.errors.clear();
            app.pk.buildLine.clear();
            ++app.pk.rev;
            app.packMode = true;
        });
    });
}

void jobPackBuild(App& app) {
    if (!app.pk.pack) return;
    Pack start = *app.pk.pack;
    bool any = std::any_of(start.pages.begin(), start.pages.end(), [](const PackPage& p) { return p.candidate.selected; });
    if (!any) {
        app.toast(ToastKind::Warn, "Select at least one page in step 3.");
        return;
    }
    ExtractOptions opts = packExtractOptions(app.settings, start.limits);
    BuildOptions bo = app.buildOptions();
    bool cache = app.settings.cacheImages;
    app.pk.errors.clear();
    app.pk.buildLine = "Starting...";
    app.pk.buildFrac = 0.f;
    app.startJob(JobKind::Pack, "Build " + start.title, [=](JobCtx& ctx) {
        // 1. Pages. Copy the live page state into the UI.
        auto lastDone = std::make_shared<int>(0);
        PackProgressFn onPage = [ctx, lastDone](const PackProgress& p) {
            PackPageState st = PackPageState::Fetching;
            std::string err;
            for (auto& e : p.errors)
                if (e.title == p.currentTitle) err = e.error;
            if (p.done > *lastDone) st = err.empty() ? PackPageState::Done : PackPageState::Failed;
            *lastDone = p.done;
            float frac = p.total ? 0.6f * static_cast<float>(p.done) / static_cast<float>(p.total) : 0.f;
            ctx.progress(frac);
            std::string line = progressText("Pages", p), title = p.currentTitle;
            auto errors = p.errors;
            ctx.post([=](App& app) {
                app.pk.buildLine = line;
                app.pk.buildFrac = frac;
                app.pk.errors = errors;
                if (!app.pk.pack || title.empty()) return;
                for (auto& pg : app.pk.pack->pages)
                    if (pg.candidate.title == title && pg.candidate.selected) {
                        pg.state = st;
                        pg.error = err;
                    }
            });
        };
        Pack fetched = spider::fetchPackPages(start, opts, onPage, &ctx.ctl->cancelPlain);
        bool cancelled = ctx.cancelled();
        int done = 0;
        for (auto& p : fetched.pages) done += p.state == PackPageState::Done ? 1 : 0;
        auto postPack = [&](const std::string& line, bool fail) {
            ctx.post([fetched, line, fail](App& app) {
                if (app.pk.pack && app.pk.pack->id == fetched.id) {
                    // Keep title and author edits made while the job ran.
                    std::string t = app.pk.pack->title, a = app.pk.pack->author;
                    app.pk.pack = fetched;
                    app.pk.pack->title = t;
                    app.pk.pack->author = a;
                    ++app.pk.rev;
                }
                app.pk.buildLine = line;
                if (fail) app.toast(ToastKind::Error, line);
            });
        };
        if (cancelled) {
            postPack("Cancelled. Fetched pages are kept. Click \"build pack\" to continue.", false);
            return;
        }
        if (!done) {
            postPack("Build failed: no page could be fetched. Look at the errors, then try again.", true);
            return;
        }
        postPack("Pages done.", false);
        // 2. Images.
        ImageMap imgs;
        if (bo.includeImages && fetched.limits.maxImagesPerArticle > 0) imgs = fetchPackImages(ctx, fetched);
        if (ctx.cancelled()) {
            postPack("Cancelled. Fetched pages are kept. Click \"build pack\" to continue.", false);
            return;
        }
        // 3. EPUB.
        ctx.progress(0.92f);
        OutputFile f;
        try {
            f = buildPack(ctx, fetched, bo, imgs);
        } catch (const std::exception& e) {
            postPack(std::string("Build failed: ") + e.what(), true);
            return;
        }
        try {
            net::savePack(fetched, cache ? &imgs : nullptr);
            net::updatePackRecord(fetched.id, f.name, std::nullopt);
        } catch (const std::exception& e) {
            ctx.log(std::string("Could not save the pack to the library: ") + e.what());
        }
        std::string line = "Built " + f.name + " (" + humanBytes(f.bytes.size()) + "), " + std::to_string(done) +
                           " articles, " + std::to_string(imgs.size()) + " images.";
        ctx.log("Pack: " + line);
        ctx.post([fetched, f, imgs, line](App& app) mutable {
            if (!app.pk.pack || app.pk.pack->id != fetched.id) return;
            // The file has the title and author of the build. Keep later edits:
            // the next export or send builds again when they differ.
            app.pk.builtTitle = fetched.title;
            app.pk.builtAuthor = fetched.author;
            fetched.title = app.pk.pack->title;
            fetched.author = app.pk.pack->author;
            app.pk.pack = std::move(fetched);
            app.pk.file = std::move(f);
            app.pk.images = std::move(imgs);
            app.pk.buildLine = line;
            app.pk.buildFrac = 1.f;
            app.pk.step = 4;
            ++app.pk.rev;
            app.packMode = true;
            app.toast(ToastKind::Info, "pack ready: " + app.pk.file->name);
            if (app.showLibrary) jobRefreshLibrary(app);
        });
    });
}

void jobPackExport(App& app, const std::string& path) {
    if (!app.pk.pack || !app.pk.file) {
        app.toast(ToastKind::Warn, "Build the pack first (pack tab, step 4).");
        return;
    }
    PackOut o = packOut(app);
    BuildOptions bo = app.buildOptions();
    app.exportStatus = "Building...";
    app.startJob(JobKind::Export, "Export pack", [=](JobCtx& ctx) mutable {
        OutputFile f = currentFile(ctx, o, bo);
        bool ok = writeFile(path, f.bytes);
        ctx.log(ok ? "Exported " + path + " (" + humanBytes(f.bytes.size()) + ")." : "Cannot write " + path);
        recordPack(ctx, o.pack.id, f.name, localRecord(o.pack.id, f, path, ok));
        ctx.post([ok, path, n = f.bytes.size()](App& app) {
            app.exportStatus = ok ? "Saved " + path + " (" + humanBytes(n) + ")." : "Cannot write " + path;
            app.toast(ok ? ToastKind::Info : ToastKind::Error, app.exportStatus);
            if (app.showLibrary) jobRefreshLibrary(app);
        });
    });
}

void jobPackSend(App& app) {
    auto prof = app.currentProfile();
    if (!prof) {
        app.sendStatus = "Add or choose a device first.";
        return;
    }
    if (!app.pk.pack || !app.pk.file) {
        app.toast(ToastKind::Warn, "Build the pack first (pack tab, step 4).");
        return;
    }
    PackOut o = packOut(app);
    BuildOptions bo = app.buildOptions();
    DeviceProfile p = *prof;
    app.lastError.reset();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Preparing the pack"};
    app.startJob(JobKind::Send, "Send pack to " + p.name, [=](JobCtx& ctx) mutable {
        OutputFile f = currentFile(ctx, o, bo);
        TransferResult res = sendWithProgress(ctx, p, f);
        recordPack(ctx, o.pack.id, f.name, makeRecord(o.pack.id, p, f, res));
        postSendResult(ctx, res, f.name, p.name);
    });
}

void jobPackOpen(App& app, const std::string& id) {
    BuildOptions bo = app.buildOptions();
    app.startJob(JobKind::Library, "Open pack", [=](JobCtx& ctx) {
        auto s = loadStored(ctx, id, bo);
        if (!s) return;
        ctx.post([s = std::move(*s)](App& app) mutable {
            app.pk.pack = std::move(s.pack);
            app.pk.images = std::move(s.images);
            app.pk.builtTitle = app.pk.pack->title;
            app.pk.builtAuthor = app.pk.pack->author;
            app.pk.fileName = s.fileName;
            app.pk.file = std::move(s.file);
            app.pk.previewKey.clear();
            app.pk.step = 4;
            app.pk.buildLine = "Opened from the library.";
            ++app.pk.rev;
            app.packMode = true;
            app.sourceTab = SourceTab::Pack;
            app.selectTab = true;
            app.transfer = TransferProgress{};
            app.toast(ToastKind::Info, "opened pack: " + app.pk.pack->title);
        });
    });
}

void jobPackReexport(App& app, const std::string& id, const std::string& path) {
    BuildOptions bo = app.buildOptions();
    app.startJob(JobKind::Export, "Re-export pack", [=](JobCtx& ctx) {
        auto s = loadStored(ctx, id, bo);
        if (!s) return;
        bool ok = writeFile(path, s->file.bytes);
        ctx.log(ok ? "Re-exported " + path : "Cannot write " + path);
        recordPack(ctx, id, s->file.name, localRecord(id, s->file, path, ok));
        ctx.post([ok, path](App& app) {
            app.statusMsg = ok ? "Saved " + path : "Cannot write " + path;
            app.toast(ok ? ToastKind::Info : ToastKind::Error, app.statusMsg);
            jobRefreshLibrary(app);
        });
    });
}

void jobPackSendStored(App& app, const std::string& id) {
    auto prof = app.currentProfile();
    if (!prof) {
        app.statusMsg = "Add or choose a device first.";
        app.toast(ToastKind::Warn, app.statusMsg);
        return;
    }
    BuildOptions bo = app.buildOptions();
    DeviceProfile p = *prof;
    app.lastError.reset();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Preparing the pack"};
    app.startJob(JobKind::Send, "Send pack to " + p.name, [=](JobCtx& ctx) {
        auto s = loadStored(ctx, id, bo);
        if (!s) return;
        TransferResult res = sendWithProgress(ctx, p, s->file);
        recordPack(ctx, id, s->file.name, makeRecord(id, p, s->file, res));
        postSendResult(ctx, res, s->file.name, p.name);
    });
}

} // namespace ks::app
