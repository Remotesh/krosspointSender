// "Send a file": any file from disk goes to the reader's SD card as it is,
// over the transport of the current device. The main use is a CrossPoint
// firmware image (firmware.bin) to "/" for the reader's Settings > System >
// SD Card Firmware Update screen. The rules (size, Bluetooth, name, folder)
// and the history are in net/file_send.cpp; this file is the window and the job.
#include "app/ui.h"

#include "net/file_send.h"

#include <imgui.h>

#include <filesystem>
#include <system_error>

namespace ks::app {

namespace {

namespace fs = std::filesystem;

SendFileItem makeItem(const std::string& path) {
    SendFileItem it;
    it.path = path;
    it.name = net::fileBaseName(path);
    std::error_code ec;
    auto n = fs::file_size(fs::u8path(path), ec);
    if (ec) it.readable = false;
    else it.size = static_cast<uint64_t>(n);
    return it;
}

void loadRecent(App& app) {
    try {
        app.sf.recent = net::listFileSends();
    } catch (const std::exception& e) {
        app.addLog(std::string("file sends: ") + e.what());
    }
    app.sf.recentLoaded = true;
}

std::string profileFolderOf(const App& app) {
    auto p = app.currentProfile();
    return net::normalizeFolder(p ? p->targetFolder : std::string("/"));
}

// The checks of every listed file for this method.
net::FileSendCheck checkAll(const App& app, TransferMethod m) {
    net::FileSendCheck all;
    for (auto& it : app.sf.items) {
        if (!it.readable) {
            all.problems.push_back(it.name + ": cannot read the file.");
            continue;
        }
        net::FileSendCheck c = net::checkFileSend(m, it.name, it.size);
        std::string pre = app.sf.items.size() > 1 ? it.name + ": " : std::string();
        for (auto& s : c.problems) all.problems.push_back(pre + s);
        for (auto& s : c.warnings) {
            bool dup = false;
            for (auto& w : all.warnings) dup = dup || w == s;
            if (!dup) all.warnings.push_back(pre.empty() || s == net::kBleFirmwareNote ? s : pre + s);
        }
    }
    return all;
}

} // namespace

void openSendFile(App& app, const std::vector<std::string>& paths, bool dropped) {
    SendFileUi& sf = app.sf;
    bool sending = app.busy(JobKind::Send);
    // A new set of files when the window was closed or the last run ended.
    if (!sf.open || sf.finished) {
        if (!sending) {
            sf.items.clear();
            sf.lines.clear();
            sf.finished = false;
            sf.cancelled = false;
            sf.firmwareSent = false;
            sf.sent = sf.failed = 0;
        }
        sf.dropped = dropped;
    }
    if (!sf.open || sf.folder.empty()) {
        sf.profileFolder = profileFolderOf(app);
        sf.folder = sf.profileFolder;
    }
    if (!sending) {
        for (auto& p : paths) {
            bool have = false;
            for (auto& it : sf.items) have = have || it.path == p;
            if (!have) sf.items.push_back(makeItem(p));
        }
    } else if (!paths.empty()) {
        app.toast(ToastKind::Warn, "a transfer runs; add the files when it ends");
    }
    sf.dropped = sf.dropped || dropped;
    sf.open = true;
    sf.focus = true;
    if (!sf.recentLoaded) loadRecent(app);
}

void chooseFilesToSend(App& app) {
    bool supported = true;
    std::vector<std::string> paths = openFilesDialog("Send a file to the reader", &supported);
    if (!supported) {
        app.toast(ToastKind::Warn,
                  "no file dialog on this platform: drop the file on the window, or use the CLI (--send <file>)");
        openSendFile(app, {});
        return;
    }
    if (paths.empty()) return;   // cancelled
    openSendFile(app, paths);
}

void jobSendRawFiles(App& app) {
    auto prof = app.currentProfile();
    if (!prof) {
        app.toast(ToastKind::Warn, "add or choose a device first");
        return;
    }
    if (app.busy(JobKind::Send) || app.sf.items.empty()) return;
    DeviceProfile p = *prof;
    std::string folder = app.sf.folder;
    std::vector<SendFileItem> items = app.sf.items;
    app.sf.lines.clear();
    app.sf.sent = app.sf.failed = 0;
    app.sf.finished = false;
    app.sf.cancelled = false;
    app.sf.firmwareSent = false;
    app.lastError.reset();
    app.transfer = TransferProgress{TransferState::Connecting, -1, 0, 0, "Connecting"};
    std::string label = items.size() == 1 ? "Send " + items[0].name : "Send " + std::to_string(items.size()) + " files";
    app.startJob(JobKind::Send, label + " to " + p.name, [p, folder, items](JobCtx& ctx) {
        auto ctl = ctx.ctl;
        auto sh = ctx.sh;
        const size_t n = items.size();
        for (size_t i = 0; i < n; ++i) {
            const SendFileItem& it = items[i];
            if (ctx.cancelled()) {
                ctx.post([](App& app) { app.sf.cancelled = true; });
                break;
            }
            TransferResult res;
            OutputFile f = net::makeRawFile(it.name, std::string());
            ctx.log("Send file " + it.path + " (" + humanBytes(it.size) + ") to " + p.name + " via " +
                    toString(p.method) + " at " + deviceWhere(p) + ", folder " +
                    net::withFolder(p, folder).targetFolder);
            if (it.size > net::kMaxSendFileBytes) {
                // Do not read a huge file into memory; the size rule refuses it.
                net::FileSendCheck c = net::checkFileSend(p.method, it.name, it.size);
                res.error = TransferError{TransferErrorCode::Unsupported,
                                          c.problems.empty() ? "The file is too large." : c.problems.back()};
            } else {
                bool ok = false;
                f.bytes = readFile(it.path, &ok);
                if (!ok) {
                    res.error = TransferError{TransferErrorCode::Unknown, "Cannot read " + it.path};
                } else {
                    ProgressFn onProgress = [ctl, sh, i, n](const TransferProgress& tp) {
                        if (tp.fraction >= 0)
                            ctl->progress =
                                static_cast<float>((static_cast<double>(i) + tp.fraction) / static_cast<double>(n));
                        sh->post([tp](App& app) { app.transfer = tp; });
                    };
                    res = net::sendRawFile(net::transport(p.method), p, folder, f, onProgress, &ctl->cancelPlain);
                }
            }
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
            TransferRecord rec = net::makeFileSendRecord(p, f, res);
            if (!res.ok) rec.bytes = it.size;   // f.bytes is empty when the file was not read
            try {
                net::addFileSend(rec);
            } catch (const std::exception& e) {
                ctx.log(std::string("file sends: ") + e.what());
            }
            postSendResult(ctx, res, it.name, p.name, p.method);
            bool fw = res.ok && net::isFirmwareName(it.name);
            std::string line = res.ok ? "sent " + res.remotePath + " (" + humanBytes(res.bytes) + ")"
                                      : "failed " + it.name + ": " + (res.error ? res.error->message : std::string("unknown error"));
            bool wasCancel = res.error && res.error->code == TransferErrorCode::Cancelled;
            ctx.post([line, ok = res.ok, fw, wasCancel](App& app) {
                app.sf.lines.push_back(line);
                if (ok) ++app.sf.sent;
                else ++app.sf.failed;
                if (fw) app.sf.firmwareSent = true;
                if (wasCancel) app.sf.cancelled = true;
            });
            if (wasCancel) break;
        }
        ctx.post([](App& app) {
            app.sf.finished = true;
            loadRecent(app);
            if (app.sf.firmwareSent) app.toast(ToastKind::Info, net::kFirmwareHint);
        });
    });
}

void drawSendFileWindow(App& app) {
    SendFileUi& sf = app.sf;
    if (!sf.open) return;
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 44.f, em * 32.f), ImGuiCond_FirstUseEver);
    if (sf.focus) {
        ImGui::SetNextWindowFocus();
        sf.focus = false;
    }
    if (!ImGui::Begin("Send a file", &sf.open)) {
        ImGui::End();
        return;
    }
    bool sending = app.busy(JobKind::Send);
    auto prof = app.currentProfile();

    if (sf.dropped)
        ui::Hint("This file is not an article (html, md, txt, epub, url), so the app does not convert it. "
                 "Send it to the reader as it is?");

    // Device.
    if (prof) {
        ImGui::TextDisabled("device");
        ImGui::SameLine();
        ImGui::Text("%s  (%s, %s)", prof->name.c_str(), methodLabel(prof->method).c_str(), deviceWhere(*prof).c_str());
    } else {
        ImGui::TextColored(theme::Warn, "No device. Add or choose a device first.");
        ImGui::SameLine();
        if (ImGui::SmallButton("devices...")) openDevicesWindow(app);
    }
    ui::SectionGap();

    // Files.
    ui::HeadingText("files");
    ImGui::SameLine();
    ImGui::BeginDisabled(sending);
    if (ImGui::SmallButton("add files...")) chooseFilesToSend(app);
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) {
        sf.items.clear();
        sf.lines.clear();
        sf.finished = false;
        sf.firmwareSent = false;
        sf.dropped = false;
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    if (sf.items.empty()) {
        ui::Hint("No file yet. Press \"add files...\" or drop files on the window.");
    } else if (ImGui::BeginTable("##files", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, em * 6.f);
        ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthFixed, em * 1.5f);
        int remove = -1;
        for (size_t i = 0; i < sf.items.size(); ++i) {
            SendFileItem& it = sf.items[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (sf.items.size() == 1 && !sending) {
                ImGui::SetNextItemWidth(-1);
                ui::InputStr("##name", it.name);
                ui::Tip(("the name on the reader. from: " + it.path).c_str());
            } else {
                ImGui::TextUnformatted(it.name.c_str());
                ui::Tip(it.path.c_str());
            }
            ImGui::TableNextColumn();
            if (it.readable) ImGui::TextUnformatted(humanBytes(it.size).c_str());
            else ImGui::TextColored(theme::Error, "unreadable");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(sending);
            if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove >= 0) sf.items.erase(sf.items.begin() + remove);
    }
    ui::SectionGap();

    // Destination folder.
    ui::Heading("destination folder");
    std::string profFolder = profileFolderOf(app);
    ImGui::BeginDisabled(sending);
    ImGui::SetNextItemWidth(em * 16.f);
    ui::InputStr("##folder", sf.folder);
    ImGui::SameLine();
    if (ImGui::SmallButton("/")) sf.folder = "/";
    ui::Tip("the SD card root (the firmware update screen looks here)");
    ImGui::SameLine();
    if (ImGui::SmallButton("device folder")) sf.folder = profFolder;
    ui::Tip(("the folder of the device: " + profFolder).c_str());
    ImGui::EndDisabled();
    bool anyBin = false;
    for (auto& it : sf.items) anyBin = anyBin || net::isFirmwareName(it.name);
    std::string dest = net::normalizeFolder(trim(sf.folder).empty() ? profFolder : sf.folder);
    if (anyBin && dest != "/")
        ui::Hint("Firmware: the reader's update screen lists the files in the SD card root. Press / to send it there.");
    ImGui::TextDisabled("goes to %s<name>", dest.c_str());
    ui::SectionGap();

    // Checks.
    net::FileSendCheck chk = checkAll(app, prof ? prof->method : TransferMethod::CrossPointHttp);
    if (!chk.problems.empty()) ui::Bullets(chk.problems, &theme::Error);
    if (!chk.warnings.empty()) ui::Bullets(chk.warnings, &theme::Warn);
    if (prof && prof->method == TransferMethod::Ble) ui::Hint(net::kBleReaderHint);

    // Send.
    bool canSend = prof && !sf.items.empty() && chk.ok() && !sending;
    ImGui::BeginDisabled(!canSend);
    std::string sendLabel = sf.items.size() > 1 ? "send " + std::to_string(sf.items.size()) + " files" : "send file";
    if (ui::PrimaryButton(sendLabel.c_str())) jobSendRawFiles(app);
    ImGui::EndDisabled();
    ui::JobRow(app, JobKind::Send);

    // Result.
    if (!sf.lines.empty() || sf.finished) {
        ui::SectionGap();
        for (auto& l : sf.lines) {
            bool bad = l.rfind("failed", 0) == 0;
            ImGui::TextColored(bad ? theme::Error : theme::Live, "%s", l.c_str());
        }
        if (sf.cancelled) ImGui::TextColored(theme::Warn, "cancelled");
        if (sf.firmwareSent) {
            ImGui::PushStyleColor(ImGuiCol_Text, theme::Amber);
            ImGui::TextWrapped("%s", net::kFirmwareHint);
            ImGui::PopStyleColor();
        }
    }

    // History.
    ui::SectionGap();
    if (ImGui::CollapsingHeader("recent file sends")) {
        if (!sf.recentLoaded) loadRecent(app);
        if (sf.recent.empty()) ui::Hint("No file sends yet.");
        size_t shown = 0;
        for (auto& r : sf.recent) {
            if (++shown > 8) break;
            ImGui::TextDisabled("%s", r.at.c_str());
            ImGui::SameLine();
            ImGui::TextColored(r.ok ? theme::Fg : theme::Error, "%s  %s  %s  %s", r.ok ? "ok" : "failed",
                               r.fileName.c_str(), humanBytes(r.bytes).c_str(), r.deviceName.c_str());
            if (!r.ok && !r.error.empty()) ui::Tip(r.error.c_str());
        }
    }
    ImGui::End();
}

} // namespace ks::app
