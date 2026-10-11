// Center column in pack mode: the article list of the pack. Each row has a
// state pill. A click on a fetched page shows its text below the list. The
// stats line sums the pack. A pill goes back to the single article.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ks::app {

namespace ui {

void PackPill(PackPageState s) {
    ImVec4 c = theme::Dim;
    switch (s) {
        case PackPageState::Done: c = theme::Live; break;
        case PackPageState::Failed: c = theme::Error; break;
        case PackPageState::Fetching: c = theme::Amber; break;
        case PackPageState::Skipped: c = theme::Muted; break;
        case PackPageState::Queued: c = theme::Dim; break;
    }
    // Fixed width, so that the titles after the pills line up.
    std::string text = "[" + toString(s) + "]";
    float w = ImGui::CalcTextSize("[fetching]").x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::TextColored(c, "%s", text.c_str());
    ImGui::SameLine(0, 0);
    ImGui::SetCursorScreenPos(ImVec2(p.x + w, p.y));
    ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
}

} // namespace ui

namespace {

const PackPage* currentPage(const App& app) {
    const Pack& pack = *app.pk.pack;
    for (auto& p : pack.pages)
        if (p.candidate.key == app.pk.previewKey && p.article) return &p;
    for (auto& p : pack.pages)
        if (packShown(p) && p.article) return &p;
    return nullptr;
}

// Build the preview lines again when the page or the pack changed.
void refreshLines(App& app, const PackPage* cur) {
    static std::string key;
    static int rev = -1;
    std::string k = cur ? cur->candidate.key : std::string();
    if (k == key && rev == app.pk.rev) return;
    key = k;
    rev = app.pk.rev;
    app.pk.previewLines = cur ? buildPreviewLines(*cur->article) : std::vector<PreviewLine>{};
    ++app.pk.previewRev;
}

void drawList(App& app, const PackPage* cur, float height) {
    ImGui::BeginChild("##packlist", ImVec2(0, height), ImGuiChildFlags_Borders);
    for (auto& p : app.pk.pack->pages) {
        if (!packShown(p)) continue;
        ImGui::PushID(&p);
        bool on = cur == &p;
        ImVec2 start = ImGui::GetCursorPos();
        ImGui::BeginDisabled(!p.article);
        if (ImGui::Selectable("##row", on, ImGuiSelectableFlags_AllowOverlap)) app.pk.previewKey = p.candidate.key;
        ImGui::EndDisabled();
        if (ui::g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("%s", p.error.empty() ? p.candidate.url.c_str() : p.error.c_str());
        ImGui::SetCursorPos(start);
        ui::PackPill(p.state);
        ImGui::SameLine();
        ImGui::TextColored(on ? theme::Amber : (p.article ? theme::Fg : theme::Dim), "%s", p.candidate.title.c_str());
        if (p.article) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d w", p.article->wordCount);
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

} // namespace

void drawPackPreview(App& app) {
    const ImGuiStyle& st = ImGui::GetStyle();
    ui::HeadingText("preview");
    auto pillW = [&](const char* t) { return ImGui::CalcTextSize(t).x + st.FramePadding.x * 2.f; };
    ui::RightAlign(pillW("[ pack ]") + (app.article ? pillW("[ article ]") + st.ItemSpacing.x : 0.f));
    ui::TabPill("pack", true);
    ui::Tip("the pack: one EPUB with many articles");
    if (app.article) {
        ImGui::SameLine();
        if (ui::TabPill("article", false)) app.packMode = false;
        ui::Tip(("show the single article: " + app.article->title).c_str());
    }
    ImGui::Separator();

    const Pack& pack = *app.pk.pack;
    PackStats s = packStats(pack, app.settings.style);
    ImGui::TextColored(theme::Amber, "%s", pack.title.c_str());
    ImGui::SameLine();
    if (s.failed) ImGui::TextDisabled("%d / %d fetched, %d failed", s.articles, s.shown, s.failed);
    else ImGui::TextDisabled("%d / %d fetched", s.articles, s.shown);

    const PackPage* cur = currentPage(app);
    refreshLines(app, cur);
    float statsH = ImGui::GetTextLineHeightWithSpacing() + 4.f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float listH = std::max(ImGui::GetFrameHeight() * 3.f, (avail.y - statsH) * 0.35f);
    drawList(app, cur, listH);

    ImVec2 rest = ImGui::GetContentRegionAvail();
    rest.y -= statsH;
    ImGui::BeginChild("##packtext", rest, ImGuiChildFlags_Borders);
    static int shownRev = -1;   // a new page starts at the top
    if (shownRev != app.pk.previewRev) {
        shownRev = app.pk.previewRev;
        ImGui::SetScrollY(0.f);
    }
    if (cur) {
        float base = st.FontSizeBase * static_cast<float>(app.settings.style.fontSizePercent) / 100.f;
        drawArticleText(*cur->article, app.pk.previewLines, app.pk.previewRev, base);
    } else {
        ui::Hint("No page is fetched yet. Build the pack in the pack tab (step 4). Then click a page to read its text.");
    }
    ImGui::EndChild();
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d articles - %d words - ~%d pages - %d img%s", s.articles, s.words,
                  app.pk.file && app.pk.file->estimatedPages ? app.pk.file->estimatedPages : s.pages, s.images,
                  s.failed ? (" - " + std::to_string(s.failed) + " failed").c_str() : "");
    ImGui::TextDisabled("%s", buf);
}

} // namespace ks::app
