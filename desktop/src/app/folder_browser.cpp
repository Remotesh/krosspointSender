// Destination folder browser of the device form ("choose destination
// folder"). CrossPoint HTTP and WebDAV: list the sub-folders of the current
// path (net::listRemoteFolder), go up, make a new folder
// (net::makeRemoteFolder), and "use this folder". The other methods keep
// the plain text field (device_setup.cpp). The send also makes a missing
// folder, so a folder that does not exist yet is not an error.
#include "app/ui.h"

#include <imgui.h>

#include <atomic>

namespace ks::app {

namespace {

std::atomic<int> g_request{0};   // the newest listing; older results are dropped

std::string parentFolder(const std::string& path) {
    auto segs = net::folderSegments(path);
    if (!segs.empty()) segs.pop_back();
    std::string out = "/";
    for (auto& s : segs) out += s + "/";
    return out;
}

std::string errorText(const TransferError& e) {
    net::Explanation ex = net::explain(e);
    return ex.title + ": " + e.message;
}

// List one folder on a worker. With fallbackToRoot, a folder that cannot be
// read shows the root, so that the user can browse from there.
void jobList(App& app, const DeviceProfile& p, const std::string& path, bool fallbackToRoot) {
    int req = ++g_request;
    app.dev.browseError.clear();
    app.dev.browseLoaded = false;
    std::string want = net::normalizeFolder(path);
    app.startJob(JobKind::Folder, "List " + want, [p, want, fallbackToRoot, req](JobCtx& ctx) {
        std::string at = want;
        std::vector<net::RemoteEntry> list;
        std::string err;
        try {
            list = net::listRemoteFolder(p, at);
        } catch (const TransferError& e) {
            if (fallbackToRoot && at != "/") {
                ctx.log(at + " cannot be read (" + e.message + "). Showing the root.");
                at = "/";
                try {
                    list = net::listRemoteFolder(p, at);
                } catch (const TransferError& e2) {
                    err = errorText(e2);
                }
            } else {
                err = errorText(e);
            }
        }
        if (err.empty()) ctx.log("Folder " + at + ": " + std::to_string(list.size()) + " item(s).");
        else ctx.log("Folder listing failed: " + err);
        ctx.post([at, list, err, req](App& app) {
            if (req != g_request.load()) return;
            app.dev.browsePath = at;
            app.dev.browseEntries = list;
            app.dev.browseError = err;
            app.dev.browseLoaded = err.empty();
        });
    });
}

void jobMake(App& app, const DeviceProfile& p, const std::string& path, const std::string& name) {
    app.startJob(JobKind::Folder, "New folder " + name, [p, path, name](JobCtx& ctx) {
        std::string err;
        try {
            net::makeRemoteFolder(p, path, name);
        } catch (const TransferError& e) {
            err = errorText(e);
        }
        std::string made = path + trim(name) + "/";
        ctx.log(err.empty() ? "Folder created: " + made : "Could not create the folder: " + err);
        ctx.post([p, made, err](App& app) {
            if (!err.empty()) {
                app.toast(ToastKind::Error, "could not create the folder: " + err);
                return;
            }
            app.toast(ToastKind::Info, "folder " + made + " created");
            app.dev.newFolderOpen = false;
            jobList(app, p, made, false);
        });
    });
}

} // namespace

void drawFolderBrowser(App& app, DeviceProfile& d) {
    DeviceFlow& f = app.dev;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("folder");
    ImGui::SameLine();
    ImGui::TextColored(theme::Amber, "%s", d.targetFolder.c_str());
    const char* toggle = f.browseOpen ? "close" : "browse...";
    ui::RightAlign(ui::ButtonWidth(toggle, true));
    if (ImGui::SmallButton(toggle)) {
        f.browseOpen = !f.browseOpen;
        if (f.browseOpen) jobList(app, d, d.targetFolder, true);
    }
    ui::Tip("choose the destination folder on the device");
    if (!f.browseOpen) return;

    bool busy = app.busy(JobKind::Folder);
    float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##browser", ImVec2(0, rowH * 9.f + ImGui::GetFrameHeightWithSpacing() * 2.f),
                      ImGuiChildFlags_Borders);
    ImGui::TextDisabled("current:");
    ImGui::SameLine();
    ImGui::TextUnformatted(f.browsePath.c_str());
    ImGui::BeginDisabled(busy || f.browsePath == "/");
    if (ImGui::SmallButton("up")) jobList(app, d, parentFolder(f.browsePath), false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton("new folder")) f.newFolderOpen = !f.newFolderOpen;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !f.browseLoaded);
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Amber);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Bg);
    if (ImGui::SmallButton("use this folder")) {
        d.targetFolder = f.browsePath;
        f.browseOpen = false;
        app.toast(ToastKind::Info, "folder: " + d.targetFolder);
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (f.newFolderOpen) {
        float bw = ui::ButtonsWidth({"create", "cancel"}, true);
        ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
        bool enter = ui::InputStrHint("##newfolder", "folder name", f.newFolderName, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || trim(f.newFolderName).empty());
        if (ImGui::SmallButton("create") || (enter && !busy)) jobMake(app, d, f.browsePath, f.newFolderName);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("cancel##newfolder")) f.newFolderOpen = false;
    }
    ImGui::Separator();
    if (busy) {
        ui::Spinner();
        ImGui::SameLine();
        ImGui::TextDisabled("reading the folder...");
    } else if (!f.browseError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Error);
        ImGui::TextWrapped("%s", f.browseError.c_str());
        ImGui::PopStyleColor();
    } else if (f.browseLoaded) {
        int dirs = 0;
        for (auto& e : f.browseEntries) dirs += e.isDirectory && e.name[0] != '.' ? 1 : 0;
        int other = static_cast<int>(f.browseEntries.size()) - dirs;
        ImGui::TextDisabled("%d folder(s), %d other item(s).", dirs, other);
        if (!dirs) ui::Hint("No sub-folders.");
        std::string next;
        for (auto& e : f.browseEntries) {
            if (!e.isDirectory || e.name[0] == '.') continue;
            std::string label = e.name + "/";
            if (ImGui::Selectable(label.c_str())) next = f.browsePath + e.name + "/";
        }
        if (!next.empty()) jobList(app, d, next, false);
    }
    ImGui::EndChild();
}

} // namespace ks::app
