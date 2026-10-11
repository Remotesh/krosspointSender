// The [ pack ] tab of the source column: the spider pack wizard.
// Steps: 1 root, 2 scan, 3 select (pack_select.cpp), 4 build, 5 done.
// Step 1 takes a Wikipedia URL or title (with a language), or the current
// article and its links (web pack). Step 2 sets the depth and the limits
// and runs the scan. Step 4 fetches the pages and the images and makes the
// EPUB. Step 5 shows the result. The pack is then the output.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>

namespace ks::app {

namespace {

const char* const kSteps[] = {"root", "scan", "select", "build", "done"};

bool sameLimits(const PackLimits& a, const PackLimits& b) {
    return a.depth == b.depth && a.maxArticles == b.maxArticles && a.maxImagesPerArticle == b.maxImagesPerArticle &&
           a.maxTotalEpubSizeMB == b.maxTotalEpubSizeMB && a.requestDelayMs == b.requestDelayMs &&
           a.includeReferences == b.includeReferences && a.includeInfobox == b.includeInfobox &&
           a.sameDomainOnly == b.sameDomainOnly;
}

bool reachable(const App& app, int i) {
    const PackUi& k = app.pk;
    return i == 0 || (i == 1 && k.hasRoot) || ((i == 2 || i == 3) && k.pack) || (i == 4 && k.file);
}

// "[ root ] [ scan ] ..." The current step is amber. A step that cannot
// open yet is dim.
void drawStepStrip(App& app) {
    const ImGuiStyle& st = ImGui::GetStyle();
    float avail = ImGui::GetContentRegionAvail().x;
    float full = 0.f;
    for (int i = 0; i < 5; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "[ %d %s ]", i + 1, kSteps[i]);
        full += ImGui::CalcTextSize(buf).x + st.FramePadding.x * 2.f + st.ItemSpacing.x;
    }
    bool compact = full > avail;
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, compact ? st.ItemSpacing.x * 0.5f : st.ItemSpacing.x);
        char label[32];
        std::snprintf(label, sizeof label, compact ? "%d" : "%d %s", i + 1, kSteps[i]);
        bool can = reachable(app, i);
        ImGui::BeginDisabled(!can && i != app.pk.step);
        ImGui::PushID(i);
        if (ui::TabPill(label, app.pk.step == i, compact) && can) app.pk.step = i;
        ImGui::PopID();
        ImGui::EndDisabled();
        ui::Tip(kSteps[i]);
    }
}

void stepRoot(App& app) {
    PackUi& k = app.pk;
    ui::Hint("A pack is one EPUB with a root article and the articles it links to. Links between them work on the "
             "reader.");
    if (k.wikiInput.empty() && app.article && extract::isWikipediaUrl(app.article->sourceUrl))
        k.wikiInput = app.article->sourceUrl;
    if (ImGui::RadioButton("wikipedia article", !k.webSource)) k.webSource = false;
    ImGui::BeginDisabled(k.webSource);
    ImGui::SetNextItemWidth(-1);
    bool enter = ui::InputStrHint("##packwiki", "https://en.wikipedia.org/wiki/Steam_engine or Steam engine",
                                  k.wikiInput, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.f);
    ui::InputStr("language##pack", k.wikiLang);
    ui::Tip("Used when you type a title. A URL sets its own language.");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!app.article);
    if (ImGui::RadioButton("current article + its links", k.webSource)) k.webSource = true;
    ImGui::EndDisabled();
    ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    ui::Hint(app.article ? app.article->title.c_str() : "load an article first");
    ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
    if (k.webSource && !app.article) k.webSource = false;
    ui::SectionGap();
    if (ui::PrimaryButton("next", ImGui::GetFrameHeight() * 1.3f) || (enter && !k.webSource)) {
        std::string err = packSetRoot(app);
        if (err.empty()) k.step = 1;
        else app.toast(ToastKind::Warn, err);
    }
}

bool intField(const char* label, int& v, int lo, int hi) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.f);
    int x = v;
    if (!ImGui::InputInt(label, &x, 0, 0)) return false;
    v = std::clamp(x, lo, hi);
    return true;
}

void stepScan(App& app) {
    PackUi& k = app.pk;
    if (!k.hasRoot) {
        ui::Hint("Choose a root first.");
        return;
    }
    ImGui::TextColored(theme::Amber, "%s", k.rootTitle.c_str());
    ui::TextMiddleEllipsis(k.rootUrl, ImGui::GetContentRegionAvail().x, theme::Dim);
    bool busy = app.busy(JobKind::Pack);
    ImGui::BeginDisabled(busy);
    static const std::vector<std::pair<int, std::string>> depths = {
        {0, "0 - root only"}, {1, "1 - root + its links"}, {2, "2 - links of links"}};
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.f);
    ui::EnumCombo("depth", k.limits.depth, depths);
    if (k.limits.depth == 2)
        ImGui::TextColored(theme::Amber, "%s", "Depth 2 reads many pages and can take minutes. Keep \"max articles\" small.");
    if (ImGui::TreeNodeEx("limits", ImGuiTreeNodeFlags_DefaultOpen)) {
        intField("max articles", k.limits.maxArticles, 1, 500);
        intField("max images per article", k.limits.maxImagesPerArticle, 0, 50);
        intField("max epub size (mb)", k.limits.maxTotalEpubSizeMB, 1, 500);
        intField("delay between requests (ms)", k.limits.requestDelayMs, 0, 10000);
        ImGui::Checkbox("include references", &k.limits.includeReferences);
        ImGui::Checkbox("include infobox", &k.limits.includeInfobox);
        if (k.rootKind == PackSourceKind::Web) ImGui::Checkbox("same site only", &k.limits.sameDomainOnly);
        ImGui::TreePop();
    }
    ImGui::EndDisabled();
    ui::SectionGap();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("scan")) jobPackScan(app);
    ImGui::EndDisabled();
    ui::Tip("read the root page and collect its links");
    ui::JobRow(app, JobKind::Pack);
    if (!k.scanLine.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, k.scanFailed ? theme::Error : theme::Dim);
        ImGui::TextWrapped("%s", k.scanLine.c_str());
        ImGui::PopStyleColor();
    }
}

void stepBuild(App& app) {
    PackUi& k = app.pk;
    if (!k.pack) {
        ui::Hint("Scan first.");
        return;
    }
    ui::Hint("The app fetches each page from Wikipedia (or the site), one at a time, then the images, then makes "
             "one EPUB.");
    bool busy = app.busy(JobKind::Pack);
    ImGui::BeginDisabled(busy);
    if (ui::PrimaryButton("build pack", ImGui::GetFrameHeight() * 1.3f)) jobPackBuild(app);
    ImGui::EndDisabled();
    ui::JobRow(app, JobKind::Pack);
    if (!k.buildLine.empty()) ImGui::TextWrapped("%s", k.buildLine.c_str());
    if (busy) ImGui::ProgressBar(k.buildFrac, ImVec2(-1, ImGui::GetFontSize() * 0.4f), "");
    for (auto& e : k.errors) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Error);
        ImGui::TextWrapped("%s: %s", e.title.c_str(), e.error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
    for (auto& p : k.pack->pages) {
        if (!packShown(p)) continue;
        ImGui::PushID(&p);
        ui::PackPill(p.state);
        ImGui::SameLine();
        ImGui::TextUnformatted(p.candidate.title.c_str());
        if (!p.error.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(theme::Error, "- %s", p.error.c_str());
        }
        ImGui::PopID();
    }
}

void stepDone(App& app) {
    PackUi& k = app.pk;
    if (!k.pack || !k.file) {
        ui::Hint("Build the pack first (step 4).");
        return;
    }
    PackStats s = packStats(*k.pack, app.settings.style);
    std::string name = packFinalName(app);
    ImGui::TextColored(theme::Live, "pack ready: %s (%s)", name.c_str(), humanBytes(k.file->bytes.size()).c_str());
    int pages = k.file->estimatedPages ? k.file->estimatedPages : s.pages;
    ImGui::TextWrapped("%d articles - %d words - ~%d pages on X4 Pro - %d images", s.articles, s.words, pages,
                       static_cast<int>(k.images.size()));
    if (s.failed)
        ImGui::TextColored(theme::Error, "%d page(s) failed. The EPUB lists them under \"Missing linked articles\".",
                           s.failed);
    ui::Hint("The pack is now the output. Edit the title, author and filename in the metadata column. Use send in "
             "the top bar or export below.");
    ui::SectionGap();
    ImGui::BeginDisabled(app.busy(JobKind::Export));
    if (ImGui::Button("export epub...")) exportNow(app);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("change pages")) k.step = 2;
    ImGui::SameLine();
    if (ImGui::Button("new pack")) packRestart(app);
    ui::JobRow(app, JobKind::Export);
    if (!app.exportStatus.empty() && !app.busy(JobKind::Export))
        ui::TextMiddleEllipsis(app.exportStatus, ImGui::GetContentRegionAvail().x, theme::Dim);
}

} // namespace

void drawPackWizard(App& app) {
    PackUi& k = app.pk;
    // New defaults in the settings apply before a scan starts.
    static PackLimits lastDefaults = app.settings.packLimits;
    if (!k.limitsSet) {
        k.limits = app.settings.packLimits;
        k.limitsSet = true;
    }
    if (!sameLimits(lastDefaults, app.settings.packLimits)) {
        lastDefaults = app.settings.packLimits;
        if (k.step <= 1 && !app.busy(JobKind::Pack)) k.limits = lastDefaults;
    }
    ImGui::TextDisabled("pack step %d: %s", k.step + 1, kSteps[k.step]);
    drawStepStrip(app);
    ImGui::Separator();
    ImGui::BeginChild("##packstep", ImVec2(0, 0), ImGuiChildFlags_None);
    switch (k.step) {
        case 0: stepRoot(app); break;
        case 1: stepScan(app); break;
        case 2: drawPackSelectStep(app); break;
        case 3: stepBuild(app); break;
        default: stepDone(app); break;
    }
    ImGui::EndChild();
}

} // namespace ks::app
