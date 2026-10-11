// Pack wizard step 3: select the pages. A checkbox list with the title, a
// depth badge, the flags and the link count. A filter box, select all /
// none, remove a row, add a page by title or URL. The counter
// "N selected / max M" blocks a selection over the limit.
#include "app/ui.h"
#include "spider/spider.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <set>

namespace ks::app {

namespace {

int selectedCount(const Pack& p) {
    int n = 0;
    for (auto& pg : p.pages) n += pg.candidate.selected ? 1 : 0;
    return n;
}

bool matches(const PackPage& p, const std::string& filter) {
    std::string f = toLower(trim(filter));
    return f.empty() || toLower(p.candidate.title).find(f) != std::string::npos;
}

// The pack changed: the built EPUB is old now.
void touched(App& app) {
    app.pk.file.reset();
    ++app.pk.rev;
}

std::string langOf(const std::string& rootUrl) {
    auto p = rootUrl.find("://");
    if (p == std::string::npos) return "en";
    auto dot = rootUrl.find(".wikipedia.org", p);
    return dot == std::string::npos ? "en" : rootUrl.substr(p + 3, dot - p - 3);
}

// A candidate from user input: a Wikipedia title or URL, or a web URL.
// Returns an error text in err when the input is not good.
std::optional<PackCandidate> manualCandidate(const Pack& pack, const std::string& input, std::string& err) {
    std::string v = trim(input);
    if (v.empty()) {
        err = "Type a title or a URL.";
        return std::nullopt;
    }
    PackCandidate c;
    c.depth = 1;
    c.selected = true;
    c.manual = true;
    if (pack.kind == PackSourceKind::Wikipedia) {
        auto n = spider::normalizeWikipediaTitle(v, langOf(pack.rootUrl));
        if (!n) {
            err = "This is not a Wikipedia article title or URL.";
            return std::nullopt;
        }
        c.key = n->title;
        c.title = n->title;
        c.url = n->url;
        return c;
    }
    auto url = extract::normalizeUrl(v);
    if (!url) {
        err = "Type a full http(s) URL.";
        return std::nullopt;
    }
    std::string key = spider::webKey(*url);
    c.key = key.empty() ? *url : key;
    c.title = c.key;
    c.url = c.key;
    return c;
}

void addPage(App& app) {
    Pack& pack = *app.pk.pack;
    std::string err;
    auto c = manualCandidate(pack, app.pk.addInput, err);
    if (!c) {
        app.toast(ToastKind::Warn, err);
        return;
    }
    int max = pack.limits.maxArticles;
    for (auto& p : pack.pages) {
        if (p.candidate.key != c->key) continue;
        if (!p.candidate.selected && selectedCount(pack) < max) p.candidate.selected = true;
        app.toast(ToastKind::Info, "\"" + c->title + "\" is already in the list.");
        app.pk.addInput.clear();
        touched(app);
        return;
    }
    if (selectedCount(pack) >= max) {
        c->selected = false;
        app.toast(ToastKind::Warn, "Added, but not selected: the limit is " + std::to_string(max) + ".");
    }
    PackPage pg;
    pg.candidate = *c;
    pack.pages.push_back(pg);
    app.pk.addInput.clear();
    touched(app);
}

void selectAll(App& app) {
    Pack& pack = *app.pk.pack;
    int room = pack.limits.maxArticles - selectedCount(pack);
    int skipped = 0;
    for (auto& p : pack.pages) {
        if (p.candidate.selected || !matches(p, app.pk.filter)) continue;
        if (room <= 0) {
            ++skipped;
            continue;
        }
        --room;
        p.candidate.selected = true;
    }
    if (skipped)
        app.toast(ToastKind::Info, std::to_string(skipped) + " page(s) not selected: the limit is " +
                                       std::to_string(pack.limits.maxArticles) + ".");
    touched(app);
}

void selectNone(App& app) {
    Pack& pack = *app.pk.pack;
    for (auto& p : pack.pages)   // the root stays selected: the pack needs it
        if (matches(p, app.pk.filter) && p.candidate.key != pack.rootKey) p.candidate.selected = false;
    touched(app);
}

// A small dim badge "[d1]" or a flag.
void badge(const char* text, const ImVec4& color, const char* tip) {
    ImGui::SameLine();
    ImGui::TextColored(color, "[%s]", text);
    ui::Tip(tip);
}

} // namespace

void drawPackSelectStep(App& app) {
    PackUi& k = app.pk;
    if (!k.pack) {
        ui::Hint("Scan first.");
        return;
    }
    Pack& pack = *k.pack;
    bool busy = app.busy(JobKind::Pack);
    ImGui::SetNextItemWidth(-1);
    ui::InputStrHint("##packfilter", "filter by title", k.filter);
    if (ImGui::SmallButton("select all")) selectAll(app);
    ImGui::SameLine();
    if (ImGui::SmallButton("select none")) selectNone(app);
    int n = selectedCount(pack);
    int max = pack.limits.maxArticles;
    ImGui::TextColored(n >= max ? theme::Amber : theme::Dim, "%d selected / max %d - %d found", n, max,
                       static_cast<int>(pack.pages.size()));

    // The list fills the space above the add row and the next button.
    float below = ImGui::GetFrameHeightWithSpacing() * 2.f + ImGui::GetStyle().ItemSpacing.y * 2.f + 8.f;
    ImGui::BeginChild("##cands", ImVec2(0, -below), ImGuiChildFlags_Borders);
    std::string removeKey;
    bool any = false;
    ImGui::BeginDisabled(busy);
    for (auto& p : pack.pages) {
        if (!matches(p, k.filter)) continue;
        any = true;
        PackCandidate& c = p.candidate;
        ImGui::PushID(c.key.c_str());
        bool on = c.selected;
        if (ImGui::Checkbox("##sel", &on)) {
            if (on && n >= max) {
                app.toast(ToastKind::Warn, "The limit is " + std::to_string(max) +
                                               " pages. Clear a page first, or raise \"max articles\" in step 2.");
            } else {
                c.selected = on;
                touched(app);
            }
        }
        ImGui::SameLine();
        ImGui::TextColored(c.selected ? theme::Fg : theme::Dim, "%s", c.title.c_str());
        ui::Tip(c.url.c_str());
        char d[8];
        std::snprintf(d, sizeof d, "d%d", c.depth);
        badge(d, theme::Dim, "depth");
        if (c.disambiguation) badge("disambig", theme::Amber, "a disambiguation page");
        if (c.redirect) badge("redirect", theme::Dim, "the link uses a redirect");
        if (c.manual) badge("added", theme::Live, "added by hand");
        if (c.linkCount) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d link%s", c.linkCount, c.linkCount == 1 ? "" : "s");
            ui::Tip("pages that link here");
        }
        ImGui::SameLine();
        if (c.key == pack.rootKey) {
            ImGui::TextColored(theme::Amber, "[root]");
        } else if (ImGui::SmallButton("x")) {
            removeKey = c.key;
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!any) ui::Hint("No page matches the filter.");
    ImGui::EndChild();
    if (!removeKey.empty()) {
        pack.pages.erase(std::remove_if(pack.pages.begin(), pack.pages.end(),
                                        [&](const PackPage& p) { return p.candidate.key == removeKey; }),
                         pack.pages.end());
        touched(app);
    }

    float bw = ui::ButtonWidth("add page", true);
    ImGui::SetNextItemWidth(-(bw + ImGui::GetStyle().ItemSpacing.x));
    bool enter = ui::InputStrHint("##packadd", "title or url of a page to add", k.addInput,
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::SmallButton("add page") || enter) addPage(app);
    if (ui::PrimaryButton("next: build")) k.step = 3;
}

} // namespace ks::app
