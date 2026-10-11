// Main columns: source | preview | right, with two drag splitters.
// A side column can collapse to a thin strip. The center column gets the
// remaining width and never goes below kCenterMin.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>

namespace ks::app {

namespace {

constexpr float kGap = 6.f;          // splitter hit zone and gap between columns
constexpr float kStrip = 24.f;       // width of a collapsed column
constexpr float kSideMin = 220.f;
constexpr float kCenterMin = 320.f;
constexpr float kLeftDefault = 380.f;
constexpr float kRightDefault = 300.f;   // metadata and output only

struct Widths {
    float left = 0, center = 0, right = 0;
};

// The widths for this frame. The prefs keep the wanted widths. When the
// window is too narrow, the right column shrinks first, then the left one.
Widths computeWidths(const App& app, float total) {
    Widths w;
    w.left = app.prefs.left_collapsed ? kStrip : app.prefs.left_width;
    w.right = app.prefs.right_collapsed ? kStrip
                                        : (app.prefs.right_width > 0.f ? app.prefs.right_width : kRightDefault);
    float room = total - kGap * 2.f - kCenterMin;
    if (w.left + w.right > room && !app.prefs.right_collapsed)
        w.right = std::max(kSideMin, room - w.left);
    if (w.left + w.right > room && !app.prefs.left_collapsed)
        w.left = std::max(kSideMin, room - w.right);
    w.center = std::max(0.f, total - kGap * 2.f - w.left - w.right);
    return w;
}

// One vertical splitter between a side column and the center. A drag
// changes the width of the side column. A double click sets the default.
// When the side column is collapsed, the splitter is only a gap.
void splitter(App& app, const char* id, float height, bool active, bool left, float x0, float total,
              const Widths& w) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (!active) {
        ImGui::Dummy(ImVec2(kGap, height));
        return;
    }
    ImGui::InvisibleButton(id, ImVec2(kGap, height));
    bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (hot) {
        float cx = p.x + kGap * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + height),
                                            ImGui::GetColorU32(theme::alpha(theme::Amber, 0.7f)), 2.f);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        if (left) app.prefs.left_width = kLeftDefault;
        else app.prefs.right_width = 0.f;   // automatic again
        app.markPrefsChanged();
        return;
    }
    if (!ImGui::IsItemActive() || ImGui::GetIO().MouseDelta.x == 0.f) return;
    float mx = ImGui::GetIO().MousePos.x;
    if (left) {
        float maxW = std::max(kSideMin, total - kGap * 2.f - kCenterMin - w.right);
        app.prefs.left_width = std::clamp(mx - x0 - kGap * 0.5f, kSideMin, maxW);
    } else {
        float maxW = std::max(kSideMin, total - kGap * 2.f - kCenterMin - w.left);
        app.prefs.right_width = std::clamp(x0 + total - mx - kGap * 0.5f, kSideMin, maxW);
    }
    app.markPrefsChanged();
}

// A collapsed column: the expand pill, then the label with one letter on
// each line. A click anywhere on the strip opens the column again.
void drawStrip(App& app, const char* label, bool left, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2.f, 4.f));
    ImGui::BeginChild(left ? "##srcStrip" : "##rightStrip", ImVec2(kStrip, height),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
    bool& collapsed = left ? app.prefs.left_collapsed : app.prefs.right_collapsed;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Amber);
    bool open = ImGui::Button(left ? ">" : "<", ImVec2(-1.f, 0.f));
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ui::Tip(left ? "open the source column (Ctrl+[)" : "open the right column (Ctrl+])");

    ImVec2 p = ImGui::GetCursorScreenPos();
    float colW = ImGui::GetContentRegionAvail().x;
    float lh = ImGui::GetTextLineHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y + lh * 0.5f;
    for (const char* c = label; *c; ++c) {
        char buf[2] = {*c, 0};
        float cw = ImGui::CalcTextSize(buf).x;
        dl->AddText(ImVec2(p.x + (colW - cw) * 0.5f, y), ImGui::GetColorU32(theme::Dim), buf);
        y += lh;
    }
    ImVec2 rest = ImGui::GetContentRegionAvail();
    if (rest.y > 1.f && ImGui::InvisibleButton("##expand", rest)) open = true;
    if (open) {
        collapsed = false;
        app.markPrefsChanged();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

} // namespace

void drawCollapsePill(App& app, bool left) {
    bool& collapsed = left ? app.prefs.left_collapsed : app.prefs.right_collapsed;
    ImGui::AlignTextToFramePadding();   // same base line as the title after it
    if (ui::TabPill(left ? "<" : ">", false)) {
        collapsed = true;
        app.markPrefsChanged();
    }
    ui::Tip(left ? "close this column (Ctrl+[)" : "close this column (Ctrl+])");
}

void drawColumns(App& app, float h) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float total = ImGui::GetContentRegionAvail().x;
    float x0 = ImGui::GetCursorScreenPos().x;
    Widths w = computeWidths(app, total);

    ImVec2 pad(st.WindowPadding.x * 0.75f, st.WindowPadding.y * 0.75f);
    const ImGuiChildFlags cf = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;

    if (app.prefs.left_collapsed) {
        drawStrip(app, "source", true, h);
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
        ImGui::BeginChild("##src", ImVec2(w.left, h), cf);
        drawSourcePanel(app);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::SameLine(0, 0);
    splitter(app, "##splitL", h, !app.prefs.left_collapsed, true, x0, total, w);
    ImGui::SameLine(0, 0);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
    ImGui::BeginChild("##prev", ImVec2(w.center, h), cf);
    drawPreviewPanel(app);
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0, 0);
    splitter(app, "##splitR", h, !app.prefs.right_collapsed, false, x0, total, w);
    ImGui::SameLine(0, 0);
    if (app.prefs.right_collapsed) {
        drawStrip(app, "details", false, h);
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pad);
        ImGui::BeginChild("##right", ImVec2(w.right, h), cf);
        drawRightPanel(app);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}

} // namespace ks::app
