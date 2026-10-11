// Preview panel: the article as plain text with headings, stats, and a
// 480x800 "device preview" frame.
#include "app/ui.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace ks::app {

namespace {

// Collapse runs of white space into one space and trim.
std::string squash(const std::string& s) {
    std::string out;
    bool space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += static_cast<char>(c);
    }
    return out;
}

} // namespace

// Split htmlToText output into lines. Mark the lines that match the toc
// entries, in order, as headings. Blank lines collapse into one.
std::vector<PreviewLine> buildPreviewLines(const Article& a) {
    std::vector<PreviewLine> out;
    std::string text = extract::htmlToText(a.bodyHtml);
    std::istringstream in(text);
    std::string line;
    size_t toc = 0;
    bool lastBlank = true;
    while (std::getline(in, line)) {
        std::string s = squash(line);
        if (s.empty()) {
            if (!lastBlank) out.push_back({"", 0});
            lastBlank = true;
            continue;
        }
        int level = 0;
        // Look a few entries ahead. One entry with odd text must not stop the rest.
        for (size_t j = toc; j < a.toc.size() && j < toc + 4; ++j) {
            if (squash(a.toc[j].text) == s) {
                level = std::clamp(a.toc[j].level, 1, 6);
                toc = j + 1;
                break;
            }
        }
        if (level && !lastBlank) out.push_back({"", 0});
        out.push_back({s, level});
        lastBlank = false;
    }
    return out;
}

namespace {

float headingScale(int level) {
    switch (level) {
        case 1: return 1.6f;
        case 2: return 1.4f;
        case 3: return 1.2f;
        default: return 1.08f;
    }
}

// Line heights for one wrap width and font size. The draw loop uses them to
// skip the lines outside the view, so a long article costs little per frame.
struct LineCache {
    int rev = -1;
    float wrap = -1.f, base = -1.f;
    std::vector<float> heights;
    std::string byline;
};

void measure(const Article& a, const std::vector<PreviewLine>& lines, int rev, LineCache& c, float wrap, float base) {
    c.rev = rev;
    c.wrap = wrap;
    c.base = base;
    c.heights.clear();
    c.heights.reserve(lines.size());
    for (auto& l : lines) {
        if (l.text.empty()) {
            ImGui::PushFont(nullptr, base);
            c.heights.push_back(ImGui::GetTextLineHeight() * 0.5f);
            ImGui::PopFont();
            continue;
        }
        ImGui::PushFont(nullptr, l.level ? base * headingScale(l.level) : base);
        c.heights.push_back(ImGui::CalcTextSize(l.text.c_str(), nullptr, false, wrap).y);
        ImGui::PopFont();
    }
    c.byline = a.author;
    if (!a.siteName.empty()) c.byline += (c.byline.empty() ? "" : " - ") + a.siteName;
    if (!a.publishedAt.empty()) c.byline += (c.byline.empty() ? "" : " - ") + a.publishedAt.substr(0, 10);
}

void wrappedText(const std::string& s, float width) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
    ImGui::TextUnformatted(s.c_str(), s.c_str() + s.size());
    ImGui::PopTextWrapPos();
}

// Draw the lines. base is the unscaled body font size. capChars > 0 limits
// the text width to that many "M" glyphs and centers the block.
void drawLines(const Article& a, const std::vector<PreviewLine>& lines, int rev, LineCache& cache, float base,
               const ImVec4& headColor, const ImVec4* bodyColor, int capChars = 0) {
    float wrap = ImGui::GetContentRegionAvail().x;
    float indent = 0.f;
    if (capChars > 0) {
        ImGui::PushFont(nullptr, base);
        float cap = ImGui::CalcTextSize(std::string(static_cast<size_t>(capChars), 'M').c_str()).x;
        ImGui::PopFont();
        if (wrap > cap) {
            indent = (wrap - cap) * 0.5f;
            wrap = cap;
        }
    }
    if (indent > 0.f) ImGui::Indent(indent);
    ImGui::PushFont(nullptr, base * 1.75f);
    ImGui::PushStyleColor(ImGuiCol_Text, headColor);
    wrappedText(a.title.empty() ? std::string("(untitled)") : a.title, wrap);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    if (cache.rev != rev || cache.wrap != wrap || cache.base != base || cache.heights.size() != lines.size())
        measure(a, lines, rev, cache, wrap, base);
    if (!cache.byline.empty()) {
        ImGui::PushFont(nullptr, base * 0.9f);
        ImGui::PushStyleColor(ImGuiCol_Text, bodyColor ? theme::alpha(*bodyColor, 0.6f) : theme::Dim);
        wrappedText(cache.byline, wrap);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::Spacing();

    const float sp = ImGui::GetStyle().ItemSpacing.y;
    const float top = ImGui::GetWindowPos().y;
    const float bottom = top + ImGui::GetWindowSize().y;
    float y = ImGui::GetCursorScreenPos().y;
    float skip = 0.f;
    int skipped = 0;
    auto flush = [&]() {
        if (!skipped) return;
        ImGui::Dummy(ImVec2(1.f, skip + sp * static_cast<float>(skipped - 1)));
        skip = 0.f;
        skipped = 0;
    };
    ImGui::PushFont(nullptr, base);
    if (bodyColor) ImGui::PushStyleColor(ImGuiCol_Text, *bodyColor);
    for (size_t i = 0; i < lines.size(); ++i) {
        const PreviewLine& l = lines[i];
        float h = cache.heights[i];
        bool visible = y + h >= top && y <= bottom;
        y += h + sp;
        if (!visible || l.text.empty()) {
            skip += h;
            ++skipped;
            continue;
        }
        flush();
        if (l.level) {
            ImGui::PushFont(nullptr, base * headingScale(l.level));
            ImGui::PushStyleColor(ImGuiCol_Text, headColor);
            wrappedText(l.text, wrap);
            ImGui::PopStyleColor();
            ImGui::PopFont();
        } else {
            wrappedText(l.text, wrap);
        }
    }
    flush();
    if (bodyColor) ImGui::PopStyleColor();
    ImGui::PopFont();
    if (indent > 0.f) ImGui::Unindent(indent);
}

LineCache g_textCache, g_deviceCache, g_packCache;

void drawDeviceFrame(App& app, ImVec2 avail) {
    // Fit a 480x800 frame (plus a bezel) into the free space, or show it
    // at 100 % (one device pixel on one screen pixel) with scroll bars.
    // The frame stays in the center of the free space.
    const float bezel = 14.f;
    float scale = 1.f;
    if (app.prefs.device_fit) {
        scale = std::min((avail.x - bezel * 2) / 480.f, (avail.y - bezel * 2) / 800.f);
        scale = std::max(scale, 0.2f);
    }
    ImVec2 screen(480.f * scale, 800.f * scale);
    float offX = std::max(0.f, (avail.x - screen.x - bezel * 2) * 0.5f);
    float offY = std::max(0.f, (avail.y - screen.y - bezel * 2) * 0.5f);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.x += offX;
    p0.y += offY;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + screen.x + bezel * 2, p0.y + screen.y + bezel * 2),
                      ImGui::GetColorU32(theme::Bezel), 12.f);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + bezel, p0.y + bezel));

    const StyleSettings& st = app.settings.style;
    float margin = static_cast<float>(st.marginEm) * 20.f * scale;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::Paper);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, theme::Paper);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, theme::alpha(theme::PaperInk, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, theme::alpha(theme::PaperInk, 0.2f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(margin + 4.f, margin + 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
    ImGui::BeginChild("##device", screen, ImGuiChildFlags_AlwaysUseWindowPadding);
    // Body text on the device is about 20 px at 100 %. PushFont takes the
    // size before the global scale factors.
    const ImGuiStyle& gs = ImGui::GetStyle();
    float px = 20.f * scale * static_cast<float>(st.fontSizePercent) / 100.f;
    float base = px / std::max(0.1f, gs.FontScaleMain * gs.FontScaleDpi);
    if (app.article)
        drawLines(*app.article, app.preview, app.articleRev, g_deviceCache, base, theme::PaperHead, &theme::PaperInk);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(4);
    // Reserve the full frame, so that the 100 % view can scroll.
    ImGui::SetCursorScreenPos(ImVec2(p0.x - offX, p0.y + screen.y + bezel * 2));
    ImGui::Dummy(ImVec2(offX + screen.x + bezel * 2, 0));
}

// The stats line. Rebuilt only when one of its inputs changes.
const std::string& statsText(App& app) {
    static std::string text;
    static int keyArticle = -1, keySettings = -1, keyImages = -1, keyFlags = -1;
    int flags = (app.imagesFetched ? 1 : 0) | (app.busy(JobKind::Images) ? 2 : 0);
    int imgs = static_cast<int>(app.images.size());
    if (keyArticle == app.articleRev && keySettings == app.settingsRev && keyImages == imgs && keyFlags == flags)
        return text;
    keyArticle = app.articleRev;
    keySettings = app.settingsRev;
    keyImages = imgs;
    keyFlags = flags;
    const Article& a = *app.article;
    output::Estimate est = output::estimateReading(a, app.settings.style);
    std::string img = std::to_string(a.images.size()) + " img";
    if (!a.images.empty()) {
        if (app.imagesFetched) img += " (" + std::to_string(imgs) + " loaded)";
        else if (flags & 2) img += " (loading)";
    }
    char buf[256];
    std::snprintf(buf, sizeof buf, "%d min read - ~%d pages - %d words - %s - %d headings", est.minutes, est.pages,
                  a.wordCount, img.c_str(), static_cast<int>(a.toc.size()));
    text = buf;
    return text;
}

} // namespace

void drawArticleText(const Article& a, const std::vector<PreviewLine>& lines, int rev, float base) {
    drawLines(a, lines, rev, g_packCache, base, theme::Accent, nullptr, 92);
}

void drawPreviewPanel(App& app) {
    if (app.packMode && app.pk.pack) {
        drawPackPreview(app);
        return;
    }
    ui::HeadingText("preview");
    const ImGuiStyle& st = ImGui::GetStyle();
    // Right side: [ text ] [ device ] [ fit ] / [ 100% ], then the preset.
    auto pillW = [&](const char* t) { return ImGui::CalcTextSize(t).x + st.FramePadding.x * 2.f; };
    float comboW = ImGui::GetFontSize() * 8.f;
    float zoomW = app.devicePreview ? pillW(app.prefs.device_fit ? "[ fit ]" : "[ 100% ]") + st.ItemSpacing.x : 0.f;
    float packW = app.pk.pack ? pillW("[ pack ]") + st.ItemSpacing.x : 0.f;
    ui::RightAlign(pillW("[ text ]") + pillW("[ device ]") + zoomW + packW + comboW + st.ItemSpacing.x * 2.f);
    if (app.pk.pack) {
        if (ui::TabPill("pack", false)) app.packMode = true;
        ui::Tip(("show the pack: " + app.pk.pack->title).c_str());
        ImGui::SameLine();
    }
    if (ui::TabPill("text", !app.devicePreview)) app.devicePreview = false;
    ui::Tip("the text that goes into the file");
    ImGui::SameLine();
    if (ui::TabPill("device", app.devicePreview)) app.devicePreview = true;
    ui::Tip("Wrap the text to a 480x800 frame, the X4 screen size.");
    if (app.devicePreview) {
        ImGui::SameLine();
        if (ui::TabPill(app.prefs.device_fit ? "fit" : "100%", false)) {
            app.prefs.device_fit = !app.prefs.device_fit;
            app.markPrefsChanged();
        }
        ui::Tip(app.prefs.device_fit ? "the frame fits the panel. click for 100 %."
                                     : "the frame is at 100 %. click to fit the panel.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(comboW);
    StylePreset p = app.settings.style.preset;
    if (ui::EnumCombo("##preset", p, ui::presetItems())) {
        std::string css = app.settings.style.customCss;
        app.settings.style = output::preset(p);
        app.settings.style.preset = p;
        app.settings.style.customCss = css;
        app.markSettingsChanged();
    }
    ui::Tip("style preset");
    ImGui::Separator();

    float statsH = ImGui::GetTextLineHeightWithSpacing() + 4.f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.y -= statsH;
    if (!app.article) {
        ImGui::BeginChild("##empty", avail, ImGuiChildFlags_Borders);
        ImGui::PushFont(nullptr, st.FontSizeBase * 1.6f);
        ImGui::TextColored(theme::Accent, "no article yet");
        ImGui::PopFont();
        ui::Hint("Fetch a URL, paste a page, or drop a file. The preview shows the text that goes into the EPUB.");
        ImGui::EndChild();
        return;
    }
    if (app.devicePreview) {
        ImGuiWindowFlags ff = app.prefs.device_fit ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                                                   : ImGuiWindowFlags_HorizontalScrollbar;
        ImGui::BeginChild("##frame", avail, 0, ff);
        drawDeviceFrame(app, ImGui::GetContentRegionAvail());
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##text", avail, ImGuiChildFlags_Borders);
        float base = st.FontSizeBase * static_cast<float>(app.settings.style.fontSizePercent) / 100.f;
        drawLines(*app.article, app.preview, app.articleRev, g_textCache, base, theme::Accent, nullptr, 92);
        ImGui::EndChild();
    }
    ImGui::TextDisabled("%s", statsText(app).c_str());
}

} // namespace ks::app
