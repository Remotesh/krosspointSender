// K-rosspointSender desktop: output module (EPUB, HTML, TXT, Markdown, ZIP).
// This is a port of the web app (src/epub/*.ts, src/converters/*.ts).
// The EPUB has the same structure as the web EPUB.
#pragma once

#include "core/types.h"

#include <optional>
#include <string>
#include <vector>

namespace ks::output {

// Style presets. The numbers are the same as the web PRESETS.
const StyleSettings& preset(StylePreset p);

// The EPUB stylesheet. The text is the same as the web epubCss().
std::string epubCss(const StyleSettings& s);

// One EPUB for one article. `images` holds image bytes keyed by ArticleImage.src.
OutputFile buildEpub(const Article& a, const BuildOptions& o, const ImageMap& images = {});

// One EPUB for many articles. Each article is one chapter file.
OutputFile buildBundleEpub(const std::vector<Article>& as, const std::string& title, const std::string& author,
                           const BuildOptions& o, const ImageMap& images = {});

// One EPUB for a spider pack (port of the web src/epub/pack.ts). Layout:
// EPUB/package.opf, nav.xhtml, toc.ncx, css/krosspoint.css,
// articles/index.xhtml, articles/<slug>.xhtml, external-links.xhtml,
// missing.xhtml, attribution.xhtml, images/<slug>_NN.jpg.
// Uses the pages with state Done and an article. Calls spider::rewriteLinks
// when a bundled page has no fileName. Over limits.maxTotalEpubSizeMB it
// removes the images of the page with the most image bytes first; when the
// EPUB is still too large it throws std::runtime_error.
OutputFile buildPackEpub(const Pack& pack, const BuildOptions& o, const ImageMap& images, const PackProgressFn& onProgress = {});

// Standalone HTML file with the EPUB CSS inline. Remote image URLs stay.
OutputFile buildHtml(const Article& a, const BuildOptions& o);

// Plain text file.
OutputFile buildTxt(const Article& a, const BuildOptions& o);

// Markdown file.
OutputFile buildMarkdown(const Article& a, const BuildOptions& o);

// ZIP: EPUB + HTML + TXT + Markdown + article.json.
OutputFile buildZip(const Article& a, const BuildOptions& o, const ImageMap& images = {});

// A tiny valid EPUB to test the device transfer: "krosspoint-test.epub".
OutputFile testEpub();

// Safe base filename (no extension). It never returns "".
std::string sanitizeFilename(const std::string& name, const FilenameRules& r = {});

// Final filename with extension from the template in `r`.
std::string makeFilename(const Article& a, OutputFormat f, const FilenameRules& r);

struct Processed {
    std::string bytes;
    std::string mime;
    int width = 0;
    int height = 0;
};

// Decodes (JPEG, PNG, GIF, BMP), scales down to maxWidth, makes gray when
// `grayscale` is true, and writes baseline JPEG (quality 1..100).
// Returns nullopt when stb cannot decode the bytes (WebP, SVG, garbage).
std::optional<Processed> processImage(const std::string& bytes, int maxWidth, bool grayscale, int quality);

struct Estimate {
    int minutes = 0;
    int pages = 1;
};

// 230 words per minute. Words per page: 210 compact, 170 comfortable,
// 120 large, 190 reference, scaled by 100 / fontSizePercent.
Estimate estimateReading(const Article& a, const StyleSettings& s);

} // namespace ks::output
