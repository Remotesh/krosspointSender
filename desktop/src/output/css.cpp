// Style presets and the EPUB stylesheet (port of epub/css.ts).
// CrossPoint reads only: text-align, text-indent, direction, font-style,
// font-weight, text-decoration, vertical-align, margin-*, padding-*, width,
// height, display, list-style-type. Units em, rem, pt.
// We add font-size, line-height and generic font-family for other readers.
#include "output/internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ks::output {

namespace {

StyleSettings makePreset(StylePreset p, int size, double margin, double lh, double para, bool links, bool hyph,
                         TableHandling tables) {
    StyleSettings s;
    s.preset = p;
    s.fontSizePercent = size;
    s.marginEm = margin;
    s.lineHeight = lh;
    s.paragraphSpacingEm = para;
    s.imageMaxWidthPx = 480;
    s.grayscaleImages = true;
    s.keepLinks = links;
    s.hyphenate = hyph;
    s.tableHandling = tables;
    s.imageQuality = 85;
    s.customCss.clear();
    return s;
}

// Formats a number for CSS: no exponent, at most 3 decimals (like JS String()).
std::string num(double v, double fallback, double lo, double hi) {
    double x = std::isfinite(v) ? std::min(hi, std::max(lo, v)) : fallback;
    x = std::round(x * 1000.0) / 1000.0;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.3f", x);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s == "-0") s = "0";
    return s;
}

} // namespace

const StyleSettings& preset(StylePreset p) {
    static const StyleSettings compact =
        makePreset(StylePreset::Compact, 90, 0.4, 1.25, 0.3, false, true, TableHandling::Linearize);
    static const StyleSettings comfortable =
        makePreset(StylePreset::Comfortable, 100, 0.8, 1.45, 0.6, true, false, TableHandling::Keep);
    static const StyleSettings large =
        makePreset(StylePreset::Large, 125, 0.8, 1.5, 0.7, false, false, TableHandling::Linearize);
    static const StyleSettings reference =
        makePreset(StylePreset::Reference, 95, 0.5, 1.35, 0.4, true, false, TableHandling::Keep);
    switch (p) {
        case StylePreset::Compact: return compact;
        case StylePreset::Comfortable: return comfortable;
        case StylePreset::Large: return large;
        case StylePreset::Reference: return reference;
    }
    return comfortable;
}

std::string epubCss(const StyleSettings& style) {
    const std::string size = num(style.fontSizePercent, 100, 50, 300);
    const std::string margin = num(style.marginEm, 0.8, 0, 5);
    const std::string lh = num(style.lineHeight, 1.45, 1, 3);
    const std::string para = num(style.paragraphSpacingEm, 0.6, 0, 5);
    const std::string hyph = style.hyphenate
                                 ? "  -webkit-hyphens: auto;\n  -epub-hyphens: auto;\n  hyphens: auto;\n"
                                 : "  -webkit-hyphens: manual;\n  -epub-hyphens: manual;\n  hyphens: manual;\n";
    const std::string align = style.hyphenate ? "justify" : "left";

    std::string css;
    css.reserve(4096);
    css += "/* k-rosspointSender stylesheet. Preset: " + toString(style.preset) + ". */\n";
    css += "html, body {\n  padding: 0;\n}\n";
    css += "body {\n  margin: " + margin + "em;\n  font-family: serif;\n  font-size: " + size +
           "%;\n  line-height: " + lh + ";\n  text-align: " + align + ";\n}\n";
    css += "p {\n  margin-top: 0;\n  margin-bottom: " + para + "em;\n  text-indent: 0;\n" + hyph + "}\n";
    css += "li, dd, blockquote {\n" + hyph + "}\n";
    css += R"(h1, h2, h3, h4, h5, h6 {
  font-weight: bold;
  text-align: left;
  text-indent: 0;
  line-height: 1.2;
  margin-top: 1em;
  margin-bottom: 0.5em;
}
h1 { font-size: 1.5em; }
h2 { font-size: 1.3em; }
h3 { font-size: 1.15em; }
h4, h5, h6 { font-size: 1em; }
blockquote {
  margin-top: 0.5em;
  margin-bottom: 0.5em;
  margin-left: 1em;
  margin-right: 0.5em;
  font-style: italic;
}
ul, ol {
  margin-top: 0;
)";
    css += "  margin-bottom: " + para + "em;\n";
    css += R"(  padding-left: 1.4em;
}
ul { list-style-type: disc; }
ol { list-style-type: decimal; }
li { margin-bottom: 0.2em; }
dt { font-weight: bold; }
dd { margin-left: 1em; margin-bottom: 0.3em; }
pre, code {
  font-family: monospace;
}
pre {
  font-size: 0.85em;
  margin-top: 0.5em;
  margin-bottom: 0.5em;
  text-align: left;
}
b, strong { font-weight: bold; }
i, em, cite { font-style: italic; }
u { text-decoration: underline; }
del { text-decoration: line-through; }
sub { vertical-align: sub; font-size: 0.75em; }
sup { vertical-align: super; font-size: 0.75em; }
a { text-decoration: underline; }
hr {
  margin-top: 1em;
  margin-bottom: 1em;
}
img{max-width:100%;height:auto}
table{max-width:100%;table-layout:fixed}
table {
  width: 100%;
  margin-top: 0.5em;
  margin-bottom: 0.5em;
}
th, td {
  padding: 0.15em;
  vertical-align: top;
  text-align: left;
}
th { font-weight: bold; }
caption { font-style: italic; }
.figure {
  margin-top: 0.5em;
  margin-bottom: 0.5em;
  text-align: center;
}
.caption {
  font-style: italic;
  text-align: center;
  font-size: 0.9em;
}
.img-alt {
  font-style: italic;
  text-align: center;
}
.table-linear p { text-align: left; }
.cover {
  margin-top: 3em;
  text-align: center;
}
.cover .title {
  font-size: 1.6em;
  margin-bottom: 1em;
  text-align: center;
}
.cover .meta {
  margin-bottom: 0.3em;
  text-align: center;
  text-indent: 0;
}
.cover ol {
  text-align: left;
}
.byline {
  font-style: italic;
  margin-bottom: 1em;
}
.source-note {
  margin-top: 2em;
  font-size: 0.85em;
  font-style: italic;
  text-align: left;
}
)";
    const std::string custom = detail::jsTrim(style.customCss);
    return custom.empty() ? css : css + "\n/* Custom CSS */\n" + custom + "\n";
}

// ----------------------------------------------------------------- reading estimate

Estimate estimateReading(const Article& a, const StyleSettings& s) {
    int words = a.wordCount;
    if (words <= 0) {
        words = 0;
        if (!a.bodyHtml.empty()) {
            xmlNodePtr root = nullptr;
            detail::Doc doc = detail::parseFragment(a.bodyHtml, &root);
            if (root) words = detail::countTextWords(detail::textOf(root));
        }
    }
    int base = 170;
    switch (s.preset) {
        case StylePreset::Compact: base = 210; break;
        case StylePreset::Comfortable: base = 170; break;
        case StylePreset::Large: base = 120; break;
        case StylePreset::Reference: base = 190; break;
    }
    const double pct = s.fontSizePercent > 0 ? s.fontSizePercent : 100;
    const double wordsPerPage = std::max(1.0, base * 100.0 / pct);
    Estimate e;
    // JS Math.round rounds .5 up; floor(x + 0.5) does the same for x >= 0.
    e.minutes = words == 0 ? 0 : std::max(1, static_cast<int>(std::floor(words / 230.0 + 0.5)));
    e.pages = std::max(1, static_cast<int>(std::ceil(words / wordsPerPage)));
    return e;
}

} // namespace ks::output
