// Log window: one line per action, with a time stamp.
#include "app/ui.h"

#include <imgui.h>

namespace ks::app {

void drawLogWindow(App& app) {
    if (!app.showLog) return;
    ImGui::SetNextWindowSize(ImVec2(720, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", &app.showLog)) {
        ImGui::End();
        return;
    }
    static std::string filter;
    if (ImGui::Button("Clear")) app.log.clear();
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        std::string all;
        for (auto& l : app.log) all += l.time + "  " + l.text + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &app.logScroll);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ui::InputStrHint("##filter", "Filter", filter);

    ImGui::BeginChild("##lines", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    std::string f = toLower(filter);
    for (auto& l : app.log) {
        if (!f.empty() && toLower(l.text).find(f) == std::string::npos) continue;
        ImGui::TextDisabled("%s", l.time.c_str());
        ImGui::SameLine();
        bool bad = l.text.find("fail") != std::string::npos || l.text.find("rror") != std::string::npos;
        if (bad) ImGui::TextColored(theme::Error, "%s", l.text.c_str());
        else ImGui::TextUnformatted(l.text.c_str());
    }
    if (app.logScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ks::app
