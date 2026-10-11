/**
 * Style presets and the EPUB stylesheet.
 * CrossPoint reads only: text-align, text-indent, direction, font-style,
 * font-weight, text-decoration, vertical-align, margin-*, padding-*, width,
 * height, display, list-style-type. Units em, rem, pt.
 * We add font-size, line-height and generic font-family for other readers.
 * No floats, columns, @font-face, animations or position.
 */
import type { StylePreset, StyleSettings } from '../types';

const BASE = {
  imageMaxWidthPx: 480,
  grayscaleImages: true,
  imageQuality: 0.85,
  customCss: '',
} as const;

export const PRESETS: Record<StylePreset, StyleSettings> = {
  compact: {
    ...BASE,
    preset: 'compact',
    fontSizePercent: 90,
    marginEm: 0.4,
    lineHeight: 1.25,
    paragraphSpacingEm: 0.3,
    keepLinks: false,
    hyphenate: true,
    tableHandling: 'linearize',
  },
  comfortable: {
    ...BASE,
    preset: 'comfortable',
    fontSizePercent: 100,
    marginEm: 0.8,
    lineHeight: 1.45,
    paragraphSpacingEm: 0.6,
    keepLinks: true,
    hyphenate: false,
    tableHandling: 'keep',
  },
  large: {
    ...BASE,
    preset: 'large',
    fontSizePercent: 125,
    marginEm: 0.8,
    lineHeight: 1.5,
    paragraphSpacingEm: 0.7,
    keepLinks: false,
    hyphenate: false,
    tableHandling: 'linearize',
  },
  reference: {
    ...BASE,
    preset: 'reference',
    fontSizePercent: 95,
    marginEm: 0.5,
    lineHeight: 1.35,
    paragraphSpacingEm: 0.4,
    keepLinks: true,
    hyphenate: false,
    tableHandling: 'keep',
  },
};

/** Formats a number for CSS: no exponent, at most 3 decimals. */
function n(v: number, fallback: number, min: number, max: number): string {
  const x = Number.isFinite(v) ? Math.min(max, Math.max(min, v)) : fallback;
  return String(Math.round(x * 1000) / 1000);
}

/** Builds the EPUB stylesheet for a style. style.customCss comes last. */
export function epubCss(style: StyleSettings): string {
  const size = n(style.fontSizePercent, 100, 50, 300);
  const margin = n(style.marginEm, 0.8, 0, 5);
  const lh = n(style.lineHeight, 1.45, 1, 3);
  const para = n(style.paragraphSpacingEm, 0.6, 0, 5);
  const hyph = style.hyphenate
    ? '  -webkit-hyphens: auto;\n  -epub-hyphens: auto;\n  hyphens: auto;\n'
    : '  -webkit-hyphens: manual;\n  -epub-hyphens: manual;\n  hyphens: manual;\n';
  const align = style.hyphenate ? 'justify' : 'left';

  const css = `/* K-rosspointSender stylesheet. Preset: ${style.preset}. */
html, body {
  padding: 0;
}
body {
  margin: ${margin}em;
  font-family: serif;
  font-size: ${size}%;
  line-height: ${lh};
  text-align: ${align};
}
p {
  margin-top: 0;
  margin-bottom: ${para}em;
  text-indent: 0;
${hyph}}
li, dd, blockquote {
${hyph}}
h1, h2, h3, h4, h5, h6 {
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
  margin-bottom: ${para}em;
  padding-left: 1.4em;
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
`;
  const custom = (style.customCss ?? '').trim();
  return custom ? `${css}\n/* Custom CSS */\n${custom}\n` : css;
}
