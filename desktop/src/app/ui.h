// Small ImGui helpers shared by the panels.
#pragma once

#include "app/app.h"

#include <imgui.h>

#include <initializer_list>
#include <string>
#include <vector>

namespace ks::app::ui {

// The colors come from theme.h (theme::Amber, theme::Accent, ...).
// applyStyle() sets these two flags from the UI prefs.
inline bool g_showTooltips = true;
inline bool g_reduceMotion = false;

bool InputStr(const char* label, std::string& s, ImGuiInputTextFlags flags = 0);
bool InputStrHint(const char* label, const char* hint, std::string& s, ImGuiInputTextFlags flags = 0);
bool InputStrMultiline(const char* label, std::string& s, const ImVec2& size, ImGuiInputTextFlags flags = 0);

// The ASCII spinner (bar, slash, dash, backslash) from weatherDesktop.
// It reads the same in every font.
char SpinnerChar();
// The spinner as one text item. Use it next to a busy label.
void Spinner();

// One "[ label ]" pill of a tab strip. The active pill is amber.
// compact drops the inner spaces ("[url]") for a narrow row.
bool TabPill(const char* label, bool active, bool compact = false);

// Width of a button with this label (small: SmallButton).
float ButtonWidth(const char* label, bool small = false);
// Width of a row of buttons, with the item spacing between them.
float ButtonsWidth(std::initializer_list<const char*> labels, bool small = false);
// Move the cursor so that an item group of this width ends at the right
// edge. sameLine keeps the current line.
void RightAlign(float width, bool sameLine = true);

// Spinner, label, progress bar and a Cancel button for each running job of
// this kind. Returns true when at least one job of this kind runs.
bool JobRow(App& app, JobKind kind);

// A combo over a fixed list. Returns true on change.
template <typename T>
bool EnumCombo(const char* label, T& value, const std::vector<std::pair<T, std::string>>& items) {
    std::string cur;
    for (auto& it : items)
        if (it.first == value) cur = it.second;
    bool changed = false;
    if (ImGui::BeginCombo(label, cur.c_str())) {
        for (auto& it : items) {
            bool sel = it.first == value;
            if (ImGui::Selectable(it.second.c_str(), sel)) {
                value = it.first;
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

const std::vector<std::pair<OutputFormat, std::string>>& formatItems();
const std::vector<std::pair<TransferMethod, std::string>>& methodItems();
const std::vector<std::pair<StylePreset, std::string>>& presetItems();
const std::vector<std::pair<FirmwareType, std::string>>& firmwareItems();

// A section title in the heading color, then a separator.
void Heading(const char* text);
// The title only. Put buttons after it, then call ImGui::Separator().
void HeadingText(const char* text);
// Text in the dim hint color, wrapped.
void Hint(const char* text);
// A bullet list of lines, wrapped.
void Bullets(const std::vector<std::string>& lines, const ImVec4* color = nullptr);
// A small text button in the accent color. It shows an underline on hover.
bool LinkButton(const char* label);
// A full-width amber button for the main action of a panel.
bool PrimaryButton(const char* label, float height = 0.f);
// "[done]" in the color of a pack page state, at a fixed width (panel_pack.cpp).
void PackPill(PackPageState s);
// A rounded "pill" with the transfer state.
void StatePill(TransferState s);
// Text that fits maxWidth. A long text loses its middle part ("ab...yz").
// The full text shows in a tooltip.
void TextMiddleEllipsis(const std::string& text, float maxWidth, const ImVec4& color);
// The one vertical gap between two sections of a panel.
void SectionGap();
// A rectangle with a dashed border.
void DashedRect(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 color, float dash = 6.f, float gap = 4.f);
// A row of checkboxes that wraps to a new line when the next one does not fit.
// Call with first = true for the first box of the row.
bool WrapCheckbox(const char* label, bool* v, bool first, const char* tip);
// A tooltip on the last item.
void Tip(const char* text);

} // namespace ks::app::ui
