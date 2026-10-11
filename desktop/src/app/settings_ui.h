// Shared parts of the settings window: the search filter, the filter-aware
// headers and the per-section reset button.
// Ported from the k-os client (same author), relicensed MIT: filt_header.
#pragma once

#include "app/ui.h"

#include <string>

namespace ks::app::settings {

// The search text. Empty means "show all".
std::string& filterText();
bool filterMatches(const char* text);

// A collapsing header that hides itself when the filter does not match
// the label or the keywords, and opens itself when it does.
bool header(const char* label, const char* keywords, ImGuiTreeNodeFlags flags = 0);

// A right-aligned "reset to defaults" button for one section. It asks once
// for confirmation. Returns true when the user confirms.
bool resetButton(const char* id);

// The sections (one function each).
void drawAppearance(App& app);
void drawShortcuts(App& app);

} // namespace ks::app::settings
