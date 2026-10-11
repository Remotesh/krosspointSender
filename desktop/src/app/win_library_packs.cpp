// Library window, packs section. Per pack: open, re-export, send, delete.
// Re-export builds the EPUB again from the stored pages and the cached
// images (when the library has them). It does not fetch again.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>

namespace ks::app {

namespace {

std::string summary(const Pack& p) {
    int done = 0, words = 0;
    for (auto& pg : p.pages)
        if (pg.state == PackPageState::Done && pg.article) {
            ++done;
            words += pg.article->wordCount;
        }
    return std::string(p.kind == PackSourceKind::Wikipedia ? "wikipedia pack" : "web pack") + " - " +
           std::to_string(done) + " articles - " + std::to_string(words) + " words";
}

} // namespace

void drawLibraryPacks(App& app) {
    std::string head = "packs (" + std::to_string(app.libraryPacks.size()) + ")###packs";
    if (!ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (app.libraryPacks.empty()) {
        ui::Hint("No packs yet. Build one in the [ pack ] tab.");
        return;
    }
    static std::string deleteId, deleteTitle;
    bool wantDelete = false;
    bool sending = app.busy(JobKind::Send);
    // One row: title and buttons, summary, saved, last send, separator.
    float rowH = ImGui::GetTextLineHeightWithSpacing() * 4.f + ImGui::GetFrameHeightWithSpacing();
    float h = std::min(rowH * 3.f, rowH * static_cast<float>(app.libraryPacks.size())) + ImGui::GetStyle().WindowPadding.y * 2.f;
    ImGui::BeginChild("##libpacks", ImVec2(0, h), ImGuiChildFlags_Borders);
    for (auto& p : app.libraryPacks) {
        ImGui::PushID(p.id.c_str());
        const net::PackRecord* rec = nullptr;
        auto it = app.packRecords.find(p.id);
        if (it != app.packRecords.end()) rec = &it->second;
        ImGui::TextColored(theme::Amber, "%s", p.title.empty() ? "(untitled pack)" : p.title.c_str());
        ui::RightAlign(ui::ButtonsWidth({"open", "re-export...", "send", "delete"}, true));
        if (ImGui::SmallButton("open")) jobPackOpen(app, p.id);
        ui::Tip("load the pack into the center column (step 5 of the wizard)");
        ImGui::SameLine();
        if (ImGui::SmallButton("re-export...")) {
            std::string name = rec && !rec->fileName.empty() ? rec->fileName : output::sanitizeFilename(p.title) + ".epub";
            if (auto path = saveFileDialog(app, name, OutputFormat::Epub)) jobPackReexport(app, p.id, *path);
        }
        ui::Tip("build the epub again from the stored pages and images");
        ImGui::SameLine();
        ImGui::BeginDisabled(sending);
        if (ImGui::SmallButton("send")) jobPackSendStored(app, p.id);
        ImGui::EndDisabled();
        ui::Tip("send the pack to the current device");
        ImGui::SameLine();
        if (ImGui::SmallButton("delete")) {
            deleteId = p.id;
            deleteTitle = p.title;
            wantDelete = true;
        }
        ImGui::TextDisabled("%s", summary(p).c_str());
        std::string saved = "saved " + p.updatedAt.substr(0, 16);
        if (rec && !rec->fileName.empty()) saved += " - " + rec->fileName;
        ImGui::TextDisabled("%s", saved.c_str());
        if (rec && !rec->transfers.empty()) {
            const TransferRecord& t = rec->transfers.back();
            ImGui::TextColored(t.ok ? theme::Live : theme::Error, "last send: %s via %s to %s, %s", t.ok ? "ok" : "failed",
                               toString(t.method).c_str(), t.deviceName.c_str(), t.at.substr(0, 16).c_str());
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (wantDelete) ImGui::OpenPopup("Delete pack?");
    if (ImGui::BeginPopupModal("Delete pack?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete the pack \"%s\" from the library?", deleteTitle.c_str());
        if (ImGui::Button("delete")) {
            std::string id = deleteId;
            app.startJob(JobKind::Library, "Delete pack", [id](JobCtx& ctx) {
                net::deletePack(id);
                ctx.log("Pack deleted: " + id);
                ctx.post([](App& app) { jobRefreshLibrary(app); });
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace ks::app
