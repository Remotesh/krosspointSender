// Right column: the metadata and output sections. The device controls are
// in the menu bar (topbar.cpp).
#include "app/ui.h"

#include <imgui.h>

namespace ks::app {

namespace {

std::vector<std::string> splitTags(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',') {
            std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur += c;
        }
    }
    return out;
}

// The filename preview. Rebuilt only when the article, the format or the
// filename rules change.
const std::string& fileNameText(App& app) {
    static std::string text;
    static int keyArticle = -1, keySettings = -1;
    static OutputFormat keyFormat = OutputFormat::Epub;
    if (keyArticle != app.articleRev || keySettings != app.settingsRev || keyFormat != app.format) {
        keyArticle = app.articleRev;
        keySettings = app.settingsRev;
        keyFormat = app.format;
        text = output::makeFilename(*app.article, app.format, app.settings.filename);
    }
    return text;
}

// Pack mode: the pack title, author and file name.
void drawPackMetadata(App& app) {
    Pack& p = *app.pk.pack;
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 4.5f);
    bool ch = false;
    ch |= ui::InputStr("title##pack", p.title);
    const char* authorHint = p.kind == PackSourceKind::Wikipedia ? "Wikipedia contributors" : "Various";
    ch |= ui::InputStrHint("author##pack", authorHint, p.author);
    std::string hint = app.pk.file ? app.pk.file->name : std::string("built name");
    ui::InputStrHint("filename##pack", hint.c_str(), app.pk.fileName);
    ui::Tip("the name of the epub on the device. empty = the built name");
    ImGui::PopItemWidth();
    if (ch) ++app.pk.rev;
    ImGui::TextDisabled("file:");
    ImGui::SameLine();
    std::string name = app.pk.file ? packFinalName(app) : std::string("(build the pack first)");
    ui::TextMiddleEllipsis(name, ImGui::GetContentRegionAvail().x, theme::Dim);
    if (app.pk.file && (p.title != app.pk.builtTitle || p.author != app.pk.builtAuthor))
        ui::Hint("The title or the author changed. Send and export build the EPUB again.");
}

void drawMetadata(App& app) {
    ui::HeadingText("metadata");
    ui::RightAlign(ImGui::CalcTextSize("[ > ]").x + ImGui::GetStyle().FramePadding.x * 2.f);
    drawCollapsePill(app, false);   // at the far right edge
    ImGui::Separator();
    if (app.packMode && app.pk.pack) {
        drawPackMetadata(app);
        return;
    }
    if (!app.article) {
        ui::Hint("Load an article to edit its title, author and tags.");
        return;
    }
    Article& a = *app.article;
    ImGui::PushItemWidth(-ImGui::GetFontSize() * 4.5f);
    bool ch = false;
    ch |= ui::InputStr("title", a.title);
    ch |= ui::InputStrHint("author", "Unknown", a.author);
    ch |= ui::InputStrHint("date", "YYYY-MM-DD", a.publishedAt);
    ch |= ui::InputStrHint("source", "https://... (optional)", a.sourceUrl);
    if (ui::InputStrHint("tags", "tag one, tag two", app.tagsInput)) {
        a.tags = splitTags(app.tagsInput);
        ch = true;
    }
    ImGui::PopItemWidth();
    if (ch) ++app.articleRev;
    ImGui::TextDisabled("file:");
    ImGui::SameLine();
    ui::TextMiddleEllipsis(fileNameText(app), ImGui::GetContentRegionAvail().x, theme::Dim);
}

void drawOutput(App& app) {
    ui::Heading("output");
    // One row: the format combo, then export at the right.
    const ImGuiStyle& st = ImGui::GetStyle();
    float exportW = ui::ButtonWidth("export...");
    bool pack = app.packMode && app.pk.pack;
    if (pack) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("epub (pack)");
        ui::RightAlign(exportW);
    } else {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - exportW - st.ItemSpacing.x);
        ui::EnumCombo("##format", app.format, ui::formatItems());
        ui::Tip("file format");
        ImGui::SameLine();
    }
    ImGui::BeginDisabled((pack ? !app.pk.file : !app.article) || app.busy(JobKind::Export));
    if (ImGui::Button("export...")) exportNow(app);
    ImGui::EndDisabled();
    ui::Tip("Build the file and save it. The article also goes into the library.");
    // One row of short toggles. They wrap when the column is narrow.
    bool ch = false;
    ch |= ui::WrapCheckbox("cover", &app.settings.includeCover, true, "include a cover page");
    ch |= ui::WrapCheckbox("note", &app.settings.includeSourceNote, false, "include the source note (url and date)");
    ch |= ui::WrapCheckbox("images", &app.settings.includeImages, false, "include the images");
    if (ch) app.markSettingsChanged();
    if (ch && pack) app.toast(ToastKind::Info, "pack: build the pack again (step 4) to apply this.");
    if (ch && !pack && app.settings.includeImages && app.article && !app.imagesFetched && !app.busy(JobKind::Images))
        jobImages(app);
    ui::JobRow(app, JobKind::Export);
    if (!app.exportStatus.empty() && !app.busy(JobKind::Export))
        ui::TextMiddleEllipsis(app.exportStatus, ImGui::GetContentRegionAvail().x, theme::Dim);
}

} // namespace

void exportNow(App& app) {
    if (app.packMode && app.pk.pack) {
        if (!app.pk.file) {
            app.toast(ToastKind::Warn, "Build the pack first (pack tab, step 4).");
            return;
        }
        if (auto path = saveFileDialog(app, packFinalName(app), OutputFormat::Epub)) jobPackExport(app, *path);
        return;
    }
    if (!app.article) return;
    std::string name = output::makeFilename(*app.article, app.format, app.settings.filename);
    if (auto path = saveFileDialog(app, name, app.format)) jobExport(app, *path);
}

void drawRightPanel(App& app) {
    drawMetadata(app);
    ui::SectionGap();
    drawOutput(app);
}

} // namespace ks::app
