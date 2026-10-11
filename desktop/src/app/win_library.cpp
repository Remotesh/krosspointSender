// Library window: saved articles, re-export, send, delete, bundle. The
// packs section is in win_library_packs.cpp.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>

namespace ks::app {

void saveToLibrary(const JobCtx& ctx, const Article& a, const OutputFile& f, const ImageMap* images,
                   const std::optional<TransferRecord>& rec) {
    try {
        LibraryEntry e;
        if (auto old = net::getEntry(a.id)) e = *old;
        else e.savedAt = nowIso();
        e.article = a;
        e.fileName = f.name;
        e.lastFormat = f.format;
        e.updatedAt = nowIso();
        net::saveEntry(e, images);
        if (rec) net::addTransfer(a.id, *rec);
    } catch (const std::exception& ex) {
        ctx.log(std::string("Library save failed: ") + ex.what());
    }
}

void jobRefreshLibrary(App& app) {
    app.startJob(JobKind::Library, "Read library", [](JobCtx& ctx) {
        auto list = net::listEntries();
        auto packs = net::listPacks();
        std::map<std::string, net::PackRecord> recs;
        for (auto& p : packs) recs[p.id] = net::getPackRecord(p.id);
        ctx.post([list = std::move(list), packs = std::move(packs), recs = std::move(recs)](App& app) mutable {
            app.library = std::move(list);
            app.libraryPacks = std::move(packs);
            app.packRecords = std::move(recs);
            app.libraryLoaded = true;
            std::set<std::string> keep;
            for (auto& e : app.library)
                if (app.librarySel.count(e.article.id)) keep.insert(e.article.id);
            app.librarySel.swap(keep);
        });
    });
}

namespace {

std::vector<std::string> selectedIds(const App& app) {
    std::vector<std::string> ids;
    for (auto& e : app.library)   // keep the list order (newest first)
        if (app.librarySel.count(e.article.id)) ids.push_back(e.article.id);
    return ids;
}

// Load one entry and its images into the main window.
void jobOpenEntry(App& app, const std::string& id) {
    app.startJob(JobKind::Library, "Open library entry", [id](JobCtx& ctx) {
        auto e = net::getEntry(id);
        if (!e) {
            ctx.log("Library entry is gone: " + id);
            return;
        }
        ImageMap imgs = net::loadImages(id);
        auto lines = buildPreviewLines(e->article);
        ctx.post([a = e->article, lines, imgs, fmt = e->lastFormat](App& app) mutable {
            app.setArticle(std::move(a), std::move(lines));
            app.images = std::move(imgs);
            app.imagesFetched = true;
            app.format = fmt;
            app.sourceStatus = "Opened from the library: " + app.article->title;
        });
    });
}

// Rebuild the entries with their stored images and write them to a path.
void jobBundle(App& app, std::vector<std::string> ids, const std::string& title, const std::string& path) {
    BuildOptions o = app.buildOptions();
    app.startJob(JobKind::Export, "Bundle " + std::to_string(ids.size()) + " articles", [=](JobCtx& ctx) {
        std::vector<Article> arts;
        ImageMap all;
        for (auto& id : ids) {
            if (ctx.cancelled()) return;
            auto e = net::getEntry(id);
            if (!e) continue;
            arts.push_back(e->article);
            for (auto& kv : net::loadImages(id)) all.insert(kv);
        }
        std::reverse(arts.begin(), arts.end());   // oldest first reads better
        OutputFile f = output::buildBundleEpub(arts, title, "", o, all);
        bool ok = writeFile(path, f.bytes);
        ctx.log(ok ? "Bundle written: " + path + " (" + humanBytes(f.bytes.size()) + ")." : "Cannot write " + path);
        ctx.post([ok, path](App& app) {
            app.exportStatus = ok ? "Bundle saved: " + path : "Cannot write " + path;
            app.statusMsg = app.exportStatus;
        });
    });
}

// Re-export one entry in its last format.
void jobReexport(App& app, const std::string& id, OutputFormat fmt, const std::string& path) {
    BuildOptions o = app.buildOptions();
    FilenameRules rules = app.settings.filename;
    app.startJob(JobKind::Export, "Re-export", [=](JobCtx& ctx) {
        auto e = net::getEntry(id);
        if (!e) return;
        ImageMap imgs = net::loadImages(id);
        OutputFile f = buildFile(e->article, fmt, o, imgs);
        f.name = output::makeFilename(e->article, fmt, rules);
        bool ok = writeFile(path, f.bytes);
        ctx.log(ok ? "Re-exported " + path : "Cannot write " + path);
        TransferRecord r;
        r.id = newUuid();
        r.articleId = id;
        r.deviceName = "Local export";
        r.method = TransferMethod::Folder;
        r.format = fmt;
        r.fileName = path;
        r.bytes = f.bytes.size();
        r.ok = ok;
        r.at = nowIso();
        saveToLibrary(ctx, e->article, f, nullptr, r);
        ctx.post([ok, path](App& app) {
            app.statusMsg = ok ? "Saved " + path : "Cannot write " + path;
            jobRefreshLibrary(app);
        });
    });
}

// Send each selected entry, one after the other, to the current device.
void jobSendEntries(App& app, std::vector<std::string> ids) {
    auto prof = app.currentProfile();
    if (!prof) {
        app.statusMsg = "Add or choose a device first.";
        return;
    }
    DeviceProfile p = *prof;
    BuildOptions o = app.buildOptions();
    FilenameRules rules = app.settings.filename;
    app.lastError.reset();
    app.startJob(JobKind::Send, "Send " + std::to_string(ids.size()) + " to " + p.name, [=](JobCtx& ctx) {
        int okCount = 0;
        std::optional<TransferError> lastErr;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (ctx.cancelled()) break;
            auto e = net::getEntry(ids[i]);
            if (!e) continue;
            OutputFile f = buildFile(e->article, e->lastFormat, o, net::loadImages(ids[i]));
            f.name = output::makeFilename(e->article, e->lastFormat, rules);
            ctx.log("Send " + f.name + " to " + p.name);
            TransferResult res = net::transport(p.method).send(p, f, [](const TransferProgress&) {}, &ctx.ctl->cancelPlain);
            ctx.progress(static_cast<float>(i + 1) / static_cast<float>(ids.size()));
            TransferRecord r;
            r.id = newUuid();
            r.articleId = ids[i];
            r.deviceId = p.id;
            r.deviceName = p.name;
            r.method = p.method;
            r.format = f.format;
            r.fileName = f.name;
            r.bytes = f.bytes.size();
            r.ok = res.ok;
            if (res.error) r.error = res.error->message;
            r.at = nowIso();
            saveToLibrary(ctx, e->article, f, nullptr, r);
            if (res.ok) ++okCount;
            else lastErr = res.error;
            ctx.log(res.ok ? "Sent " + res.remotePath : "Failed: " + r.error);
        }
        ctx.post([okCount, n = ids.size(), lastErr](App& app) {
            app.statusMsg = "Sent " + std::to_string(okCount) + " of " + std::to_string(n) + ".";
            if (lastErr) app.lastError = lastErr;
            jobRefreshLibrary(app);
        });
    });
}

} // namespace

void drawLibraryWindow(App& app) {
    if (!app.showLibrary) return;
    if (!app.libraryLoaded && !app.busy(JobKind::Library)) {
        app.libraryLoaded = true;   // one try; Refresh asks again
        jobRefreshLibrary(app);
    }
    ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Library", &app.showLibrary)) {
        ImGui::End();
        return;
    }
    static std::string bundleTitle = "Reading bundle";
    static bool confirmClear = false;
    auto ids = selectedIds(app);

    if (ImGui::Button("Refresh")) jobRefreshLibrary(app);
    ImGui::SameLine();
    ImGui::BeginDisabled(ids.empty());
    if (ImGui::Button("Open")) jobOpenEntry(app, ids.front());
    ui::Tip("Load the first selected article into the preview.");
    ImGui::SameLine();
    if (ImGui::Button("Re-export...")) {
        for (auto& e : app.library) {
            if (e.article.id != ids.front()) continue;
            std::string name = output::makeFilename(e.article, e.lastFormat, app.settings.filename);
            if (auto path = saveFileDialog(app, name, e.lastFormat)) jobReexport(app, e.article.id, e.lastFormat, *path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Send")) jobSendEntries(app, ids);
    ui::Tip("Send the selected articles to the current device.");
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        app.startJob(JobKind::Library, "Delete entries", [ids](JobCtx& ctx) {
            for (auto& id : ids) net::deleteEntry(id);
            ctx.log("Deleted " + std::to_string(ids.size()) + " library entries.");
            ctx.post([](App& app) { jobRefreshLibrary(app); });
        });
        app.librarySel.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Bundle selected into one EPUB")) ImGui::OpenPopup("Bundle");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.library.empty() && app.libraryPacks.empty());
    if (ImGui::Button("Clear all")) confirmClear = true;
    ImGui::EndDisabled();
    if (confirmClear) {
        ImGui::OpenPopup("Clear library?");
        confirmClear = false;
    }

    if (ImGui::BeginPopupModal("Bundle", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Bundle %d articles into one EPUB.", static_cast<int>(ids.size()));
        ui::InputStr("Title", bundleTitle);
        if (ImGui::Button("Save...") && !trim(bundleTitle).empty()) {
            std::string name = output::sanitizeFilename(bundleTitle, app.settings.filename) + ".epub";
            if (auto path = saveFileDialog(app, name, OutputFormat::Epub)) jobBundle(app, ids, bundleTitle, *path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Clear library?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Delete every saved article, every pack and their images?");
        if (ImGui::Button("Delete all")) {
            app.startJob(JobKind::Library, "Clear library", [](JobCtx& ctx) {
                net::clearLibrary();
                ctx.log("Library cleared.");
                ctx.post([](App& app) { jobRefreshLibrary(app); });
            });
            app.librarySel.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ui::JobRow(app, JobKind::Library);
    if (!app.statusMsg.empty()) ui::Hint(app.statusMsg.c_str());
    drawLibraryPacks(app);
    if (app.library.empty()) ui::Hint("The library is empty. Every export and send saves the article here.");

    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("lib", 5, tf)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Site", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Sends", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Updated", ImGuiTableColumnFlags_WidthFixed, 160);
        ImGui::TableHeadersRow();
        for (auto& e : app.library) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool sel = app.librarySel.count(e.article.id) > 0;
            ImGui::PushID(e.article.id.c_str());
            std::string label = e.article.title.empty() ? "(untitled)" : e.article.title;
            if (ImGui::Selectable(label.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                bool ctrl = ImGui::GetIO().KeyCtrl;
                if (!ctrl) app.librarySel.clear();
                if (ctrl && sel) app.librarySel.erase(e.article.id);
                else app.librarySel.insert(e.article.id);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) jobOpenEntry(app, e.article.id);
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.article.siteName.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatLabel(e.lastFormat).c_str());
            ImGui::TableNextColumn();
            int ok = 0;
            for (auto& t : e.transfers) ok += t.ok ? 1 : 0;
            ImGui::Text("%d/%d", ok, static_cast<int>(e.transfers.size()));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.updatedAt.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

} // namespace ks::app
