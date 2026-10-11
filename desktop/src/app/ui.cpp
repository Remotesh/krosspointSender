// Small ImGui helpers shared by the panels.
#include "app/ui.h"

#include <cstdio>

namespace ks::app::ui {

namespace {

// Grow the std::string when ImGui asks for more room.
int resizeCb(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize(static_cast<size_t>(d->BufTextLen));
        d->Buf = s->data();
    }
    return 0;
}

} // namespace

bool InputStr(const char* label, std::string& s, ImGuiInputTextFlags flags) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputText(label, s.data(), s.capacity() + 1, flags, resizeCb, &s);
}

bool InputStrHint(const char* label, const char* hint, std::string& s, ImGuiInputTextFlags flags) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextWithHint(label, hint, s.data(), s.capacity() + 1, flags, resizeCb, &s);
}

bool InputStrMultiline(const char* label, std::string& s, const ImVec2& size, ImGuiInputTextFlags flags) {
    flags |= ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputTextMultiline(label, s.data(), s.capacity() + 1, size, flags, resizeCb, &s);
}

char SpinnerChar() {
    static const char kFrames[4] = {'|', '/', '-', '\\'};
    return kFrames[static_cast<int>(ImGui::GetTime() * 8.0) & 3];
}

void Spinner() {
    char buf[2] = {SpinnerChar(), 0};
    ImGui::TextColored(theme::Amber, "%s", buf);
}

bool TabPill(const char* label, bool active, bool compact) {
    char buf[64];
    std::snprintf(buf, sizeof buf, compact ? "[%s]" : "[ %s ]", label);
    if (active) ImGui::PushStyleColor(ImGuiCol_Text, theme::Amber);
    if (compact) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.f, 0.f));
    bool pressed = ImGui::SmallButton(buf);
    if (compact) ImGui::PopStyleVar();
    if (active) ImGui::PopStyleColor();
    return pressed;
}

float ButtonWidth(const char* label, bool) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.f;
}

float ButtonsWidth(std::initializer_list<const char*> labels, bool small) {
    float w = 0;
    for (const char* l : labels) w += ButtonWidth(l, small);
    if (labels.size() > 1) w += ImGui::GetStyle().ItemSpacing.x * static_cast<float>(labels.size() - 1);
    return w;
}

void RightAlign(float width, bool sameLine) {
    if (sameLine) ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > width) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - width);
}

bool JobRow(App& app, JobKind kind) {
    bool any = false;
    for (auto& j : app.jobs) {
        if (j->kind != kind || j->done) continue;
        any = true;
        ImGui::PushID(j->id);
        Spinner();
        ImGui::SameLine();
        ImGui::TextUnformatted(j->label.c_str());
        float p = j->progress.load();
        if (p >= 0.f) {
            ImGui::SameLine();
            ImGui::ProgressBar(p, ImVec2(ImGui::GetFontSize() * 6.f, 0));
        }
        ImGui::SameLine();
        if (j->cancel) ImGui::TextDisabled("cancelling...");
        else if (ImGui::SmallButton("cancel")) j->requestCancel();
        ImGui::PopID();
    }
    return any;
}

const std::vector<std::pair<OutputFormat, std::string>>& formatItems() {
    static const std::vector<std::pair<OutputFormat, std::string>> v = {
        {OutputFormat::Epub, "EPUB"}, {OutputFormat::Txt, "TXT"}, {OutputFormat::Html, "HTML"},
        {OutputFormat::Md, "Markdown"}, {OutputFormat::Zip, "ZIP (HTML + images)"}};
    return v;
}

const std::vector<std::pair<TransferMethod, std::string>>& methodItems() {
    static std::vector<std::pair<TransferMethod, std::string>> v;
    if (v.empty())
        for (TransferMethod m : net::methods()) v.emplace_back(m, methodLabel(m));
    return v;
}

const std::vector<std::pair<StylePreset, std::string>>& presetItems() {
    static const std::vector<std::pair<StylePreset, std::string>> v = {
        {StylePreset::Compact, "Compact"}, {StylePreset::Comfortable, "Comfortable"},
        {StylePreset::Large, "Large"}, {StylePreset::Reference, "Reference"}};
    return v;
}

const std::vector<std::pair<FirmwareType, std::string>>& firmwareItems() {
    static const std::vector<std::pair<FirmwareType, std::string>> v = {
        {FirmwareType::CrossPoint, "CrossPoint"}, {FirmwareType::KReader, "K-Reader (future)"},
        {FirmwareType::Other, "Other"}};
    return v;
}

void Heading(const char* text) {
    HeadingText(text);
    ImGui::Separator();
}

void HeadingText(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::Accent, "%s", text);
}

void Hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Dim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void Bullets(const std::vector<std::string>& lines, const ImVec4* color) {
    if (color) ImGui::PushStyleColor(ImGuiCol_Text, *color);
    for (auto& l : lines) {
        ImGui::Bullet();
        ImGui::TextWrapped("%s", l.c_str());
    }
    if (color) ImGui::PopStyleColor();
}

bool LinkButton(const char* label) {
    ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += ImGui::GetStyle().FramePadding.y;
    bool pressed = ImGui::InvisibleButton(label, ImVec2(ts.x, ImGui::GetFrameHeight()));
    bool hot = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 c = ImGui::GetColorU32(hot ? theme::Amber : theme::Accent);
    dl->AddText(p, c, label);   // the label has no "##" part
    if (hot) {
        dl->AddLine(ImVec2(p.x, p.y + ts.y), ImVec2(p.x + ts.x, p.y + ts.y), c);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    return pressed;
}

bool PrimaryButton(const char* label, float height) {
    ImGui::PushStyleColor(ImGuiCol_Button, theme::Amber);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::Accent);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Bg);
    bool pressed = ImGui::Button(label, ImVec2(-1.f, height));
    ImGui::PopStyleColor(4);
    return pressed;
}

void StatePill(TransferState s) {
    ImVec4 c = s == TransferState::Complete ? theme::Live
               : s == TransferState::Failed ? theme::Error
               : s == TransferState::Idle   ? theme::Dim
                                            : theme::Amber;
    std::string t = stateLabel(s);
    ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    ImVec2 pad(8.f, 2.f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 q(p.x + ts.x + pad.x * 2, p.y + ts.y + pad.y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, 0.22f)), 20.f);
    dl->AddRect(p, q, ImGui::GetColorU32(c), 20.f);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), ImGui::GetColorU32(c), t.c_str());
    ImGui::Dummy(ImVec2(q.x - p.x, q.y - p.y));
}

void TextMiddleEllipsis(const std::string& text, float maxWidth, const ImVec4& color) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth || text.size() < 8) {
        ImGui::TextColored(color, "%s", text.c_str());
        return;
    }
    // Remove characters from the middle until the text fits.
    size_t keep = text.size();
    std::string out;
    while (keep > 4) {
        --keep;
        size_t head = (keep + 1) / 2, tail = keep / 2;
        out = text.substr(0, head) + "..." + text.substr(text.size() - tail);
        if (ImGui::CalcTextSize(out.c_str()).x <= maxWidth) break;
    }
    ImGui::TextColored(color, "%s", out.c_str());
    if (g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text.c_str());
}

void SectionGap() { ImGui::Dummy(ImVec2(0.f, ImGui::GetStyle().ItemSpacing.y)); }

void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float dash, float gap) {
    auto line = [&](ImVec2 p, ImVec2 q) {
        float len = (q.x - p.x) + (q.y - p.y);   // the lines are horizontal or vertical
        float dx = (q.x - p.x) / len, dy = (q.y - p.y) / len;
        for (float t = 0.f; t < len; t += dash + gap) {
            float e = t + dash < len ? t + dash : len;
            dl->AddLine(ImVec2(p.x + dx * t, p.y + dy * t), ImVec2(p.x + dx * e, p.y + dy * e), color, 1.f);
        }
    };
    line(a, ImVec2(b.x, a.y));
    line(ImVec2(b.x, a.y), b);
    line(ImVec2(a.x, b.y), b);
    line(a, ImVec2(a.x, b.y));
}

bool WrapCheckbox(const char* label, bool* v, bool first, const char* tip) {
    const ImGuiStyle& st = ImGui::GetStyle();
    if (!first) {
        float w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize(label, nullptr, true).x;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
    }
    bool ch = ImGui::Checkbox(label, v);
    Tip(tip);
    return ch;
}

void Tip(const char* text) {
    if (g_showTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
}

} // namespace ks::app::ui
