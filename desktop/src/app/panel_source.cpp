// Source panel: URL, Wikipedia, paste, Markdown, file. Also the drop and
// clipboard routing.
#include "app/ui.h"

#include "net/file_send.h"

#include <imgui.h>

#include <cstdio>

namespace ks::app {

namespace {

std::string extOf(const std::string& path) {
    size_t dot = path.find_last_of('.');
    size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return toLower(path.substr(dot + 1));
}

std::string baseName(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

void setTab(App& app, SourceTab t) {
    app.sourceTab = t;
    app.selectTab = true;
}

} // namespace

RawInput rawFromFile(const std::string& path, const std::string& bytes) {
    RawInput in;
    in.content = bytes;
    in.fileName = baseName(path);
    std::string ext = extOf(path);
    if (ext == "html" || ext == "htm" || ext == "xhtml") in.kind = InputKind::Html;
    else if (ext == "md" || ext == "markdown") in.kind = InputKind::Markdown;
    else if (ext == "txt") in.kind = InputKind::Text;
    else in.kind = extract::detectInputKind(bytes, *in.fileName);
    // A file that holds only a URL is not useful as text. Keep the kind.
    if (in.kind == InputKind::Url || in.kind == InputKind::Wikipedia) in.kind = InputKind::Text;
    return in;
}

void handleDropFile(App& app, const std::string& path) {
    app.addLog("Dropped file: " + path);
    // Not an article input (for example firmware.bin): offer "send as file"
    // in the send-file window instead of a convert that cannot work.
    if (!net::isArticleInputPath(path)) {
        openSendFile(app, {path}, true);
        return;
    }
    app.filePath = path;
    setTab(app, SourceTab::File);
    jobLoadFile(app, path);
}

void handleDropText(App& app, const std::string& text) {
    std::string t = trim(text);
    bool oneLine = t.find('\n') == std::string::npos;
    auto url = oneLine ? extract::normalizeUrl(t) : std::nullopt;
    app.addLog(url ? "Dropped URL: " + *url : "Dropped text (" + humanBytes(t.size()) + ").");
    if (url) {
        app.urlInput = *url;
        setTab(app, SourceTab::Url);
        jobLoadUrl(app, *url);
        return;
    }
    RawInput in;
    in.content = text;
    in.kind = extract::detectInputKind(text);
    if (in.kind == InputKind::Url || in.kind == InputKind::Wikipedia) in.kind = InputKind::Text;
    if (in.kind == InputKind::Html) {
        app.pasteHtml = text;
        setTab(app, SourceTab::Html);
    } else if (in.kind == InputKind::Markdown) {
        app.pasteMd = text;
        setTab(app, SourceTab::Markdown);
    } else {
        app.pasteText = text;
        setTab(app, SourceTab::Text);
    }
    jobExtract(app, std::move(in));
}

void pasteFromClipboard(App& app) {
    ClipboardContent c = readClipboard();
    if (!c.html.empty()) {
        app.addLog("Paste: CF_HTML " + humanBytes(c.html.size()) +
                   (c.sourceUrl.empty() ? std::string() : " from " + c.sourceUrl));
        app.pasteHtml = c.html;
        setTab(app, SourceTab::Html);
        RawInput in;
        in.kind = InputKind::Html;
        in.content = c.html;
        if (!c.sourceUrl.empty()) in.sourceUrl = c.sourceUrl;
        jobExtract(app, std::move(in));
        return;
    }
    if (trim(c.text).empty()) {
        app.sourceError = "The clipboard is empty.";
        return;
    }
    handleDropText(app, c.text);
}

namespace {

void convertBuffer(App& app, const std::string& buf, InputKind kind) {
    RawInput in;
    in.kind = kind;
    in.content = buf;
    jobExtract(app, std::move(in));
}

void drawPasteTab(App& app, std::string& buf, InputKind kind, const char* hint) {
    ui::Hint(hint);
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4.f;
    ui::InputStrMultiline("##paste", buf, ImVec2(-1, h < 120 ? 120 : h));
    ui::RightAlign(ui::ButtonsWidth({"clear", "convert"}), false);
    if (ImGui::Button("clear")) buf.clear();
    ImGui::SameLine();
    ImGui::BeginDisabled(app.busy(JobKind::Source));
    if (ImGui::Button("convert")) convertBuffer(app, buf, kind);
    ImGui::EndDisabled();
    ui::Tip("extract the article from this box (Ctrl+Enter by default)");
}

void drawUrlTab(App& app) {
    ImGui::TextDisabled("article url");
    float bw = ui::ButtonWidth("fetch");
    ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
    if (app.focusUrl) {
        ImGui::SetKeyboardFocusHere();
        app.focusUrl = false;
    }
    bool enter = ui::InputStrHint("##url", "https://example.com/article", app.urlInput,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(app.busy(JobKind::Source));
    if ((ImGui::Button("fetch") || enter) && !app.busy(JobKind::Source)) jobLoadUrl(app, app.urlInput);
    ImGui::EndDisabled();
    ui::Tip("The desktop app fetches any page. There is no CORS limit here.");
}

void drawWikiTab(App& app) {
    ImGui::TextDisabled("wikipedia article");
    ImGui::SetNextItemWidth(-1);
    bool enter = ui::InputStrHint("##wiki", "Article title or Wikipedia URL", app.wikiInput,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.f);
    ui::InputStr("language", app.wikiLang);
    ui::RightAlign(ui::ButtonWidth("fetch from wikipedia"));
    ImGui::BeginDisabled(app.busy(JobKind::Source));
    if (ImGui::Button("fetch from wikipedia") || (enter && !app.busy(JobKind::Source)))
        jobWikipedia(app, app.wikiInput, app.wikiLang);
    ImGui::EndDisabled();
    ui::Hint("Uses the MediaWiki API. Infoboxes and references follow the extract settings.");
}

void drawFileTab(App& app) {
    ImGui::TextDisabled("choose a file");
    float bw = ui::ButtonsWidth({"browse...", "load"});
    ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
    ui::InputStrHint("##file", "C:/path/article.html", app.filePath);
    ImGui::SameLine();
    if (ImGui::Button("browse...")) {
        if (auto p = openFileDialog()) {
            app.filePath = *p;
            jobLoadFile(app, *p);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("load")) jobLoadFile(app, app.filePath);
    ui::Hint("HTML, Markdown or TXT. You can also drop a file anywhere on this window.");
}

// The drop area. It fills all the free space under the inputs.
void drawDropZone() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float minH = ImGui::GetFrameHeight() * 2.f;
    if (avail.y < minH) return;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 q(p.x + avail.x, p.y + avail.y - 1.f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::DashedRect(dl, p, q, ImGui::GetColorU32(theme::Muted));
    const char* msg = "[ drop a url, text or a file anywhere ]";
    ImVec2 ts = ImGui::CalcTextSize(msg);
    if (ts.x > avail.x - 8.f) msg = "[ drop here ]";
    ts = ImGui::CalcTextSize(msg);
    dl->AddText(ImVec2(p.x + (avail.x - ts.x) * 0.5f, p.y + (q.y - p.y - ts.y) * 0.5f), ImGui::GetColorU32(theme::Dim),
                msg);
    ImGui::Dummy(ImVec2(avail.x, avail.y - 1.f));
}

struct TabDef {
    SourceTab t;
    const char* name;   // short label of the pill
    const char* tip;    // full name
};
const TabDef kTabs[] = {{SourceTab::Url, "url", "web page url"},      {SourceTab::Wikipedia, "wiki", "wikipedia"},
                        {SourceTab::Html, "html", "pasted html"},     {SourceTab::Text, "text", "plain text"},
                        {SourceTab::Markdown, "md", "markdown"},      {SourceTab::File, "file", "file on disk"},
                        {SourceTab::Pack, "pack", "spider pack: an article and its links in one epub"}};

// The "[ url ] [ wikipedia ] ..." strip (weatherDesktop). It wraps when
// the column is narrow.
void drawTabStrip(App& app) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const size_t n = sizeof kTabs / sizeof kTabs[0];
    float avail = ImGui::GetContentRegionAvail().x;
    // Keep one row: use the compact "[url]" form when "[ url ]" does not fit.
    float full = 0.f;
    for (size_t i = 0; i < n; ++i) {
        char buf[48];
        std::snprintf(buf, sizeof buf, "[ %s ]", kTabs[i].name);
        full += ImGui::CalcTextSize(buf).x + st.FramePadding.x * 2.f + (i ? st.ItemSpacing.x : 0.f);
    }
    bool compact = full > avail;
    float gap = compact ? st.ItemSpacing.x * 0.5f : st.ItemSpacing.x;
    float right = ImGui::GetCursorScreenPos().x + avail;
    for (size_t i = 0; i < n; ++i) {
        const TabDef& td = kTabs[i];
        if (i) {
            char buf[48];
            std::snprintf(buf, sizeof buf, compact ? "[%s]" : "[ %s ]", td.name);
            float w = ImGui::CalcTextSize(buf).x + (compact ? 6.f : st.FramePadding.x * 2.f);
            ImGui::SameLine(0, gap);
            if (ImGui::GetCursorScreenPos().x + w > right) ImGui::NewLine();   // very narrow: wrap
        }
        if (ui::TabPill(td.name, app.sourceTab == td.t, compact)) app.sourceTab = td.t;
        ui::Tip(td.tip);
    }
}

} // namespace

void convertCurrent(App& app) {
    if (app.busy(JobKind::Source)) return;
    switch (app.sourceTab) {
        case SourceTab::Url: jobLoadUrl(app, app.urlInput); break;
        case SourceTab::Wikipedia: jobWikipedia(app, app.wikiInput, app.wikiLang); break;
        case SourceTab::Html: convertBuffer(app, app.pasteHtml, InputKind::Html); break;
        case SourceTab::Text: convertBuffer(app, app.pasteText, InputKind::Text); break;
        case SourceTab::Markdown: convertBuffer(app, app.pasteMd, InputKind::Markdown); break;
        case SourceTab::File: jobLoadFile(app, app.filePath); break;
        case SourceTab::Pack: break;
    }
}

void fetchCurrent(App& app) {
    app.sourceTab = SourceTab::Url;
    if (trim(app.urlInput).empty()) {
        app.focusUrl = true;
        return;
    }
    if (!app.busy(JobKind::Source)) jobLoadUrl(app, app.urlInput);
}

void drawSourcePanel(App& app) {
    drawCollapsePill(app, true);
    ImGui::SameLine();
    ui::HeadingText("source");
    ui::RightAlign(ui::ButtonWidth("paste"));
    if (ImGui::Button("paste")) pasteFromClipboard(app);
    ui::Tip("Read the clipboard. A browser copy gives real HTML and the source URL.");
    ImGui::Separator();

    drawTabStrip(app);
    ui::SectionGap();
    if (app.sourceTab == SourceTab::Pack) {
        app.selectTab = false;
        drawPackWizard(app);
        return;
    }
    switch (app.sourceTab) {
        case SourceTab::Url: drawUrlTab(app); break;
        case SourceTab::Wikipedia: drawWikiTab(app); break;
        case SourceTab::Html:
            drawPasteTab(app, app.pasteHtml, InputKind::Html, "Page HTML. Paste the source of a page.");
            break;
        case SourceTab::Text:
            drawPasteTab(app, app.pasteText, InputKind::Text, "Plain text. A blank line starts a new paragraph.");
            break;
        case SourceTab::Markdown:
            drawPasteTab(app, app.pasteMd, InputKind::Markdown, "Markdown. # Title, then text.");
            break;
        case SourceTab::File: drawFileTab(app); break;
        case SourceTab::Pack: break;
    }
    app.selectTab = false;

    ui::JobRow(app, JobKind::Source);
    ui::JobRow(app, JobKind::Images);
    // One thin line: the last loaded article, or the last source state.
    if (!app.busy(JobKind::Source)) {
        if (app.article && app.sourceError.empty()) {
            ImGui::TextDisabled("last loaded");
            ImGui::SameLine();
            ImGui::TextColored(theme::Live, "%s", app.loadedAt.c_str());
            ImGui::SameLine();
            const std::string& t = app.article->title;
            ImGui::TextDisabled("%s", t.empty() ? "(untitled)" : t.c_str());
            ui::Tip(t.c_str());
        } else if (!app.sourceStatus.empty() && app.sourceError.empty()) {
            ImGui::TextDisabled("%s", app.sourceStatus.c_str());
        }
    }
    if (!app.sourceError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::Error);
        ImGui::TextWrapped("%s", app.sourceError.c_str());
        ImGui::PopStyleColor();
        if (!app.sourceFallbacks.empty()) {
            ImGui::TextUnformatted("What you can do:");
            ui::Bullets(app.sourceFallbacks);
        }
    }
    if (app.article && app.lastInput && !app.busy(JobKind::Source)) {
        if (ImGui::SmallButton("extract again")) jobExtract(app, *app.lastInput);
        ui::Tip("run the extract again with the current extract settings");
    }
    ui::SectionGap();
    drawDropZone();
}

} // namespace ks::app
