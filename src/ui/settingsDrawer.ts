/**
 * Settings drawer. All advanced settings: style, custom CSS, filename
 * rules, images, links, tables, extraction, library cache, helper URL.
 * Each change updates the store at once. The preview shows it live.
 * The settings are saved after a short delay.
 */
import type { AppSettings, ExtractOptions, FilenameRules, PackLimits, StyleSettings } from '../types';
import { resetSettings } from '../settings';
import { checkbox, createDrawer, errorMessage, field, h, replace, select, type Drawer } from './dom';
import { persistSettings, runExtract } from './actions';
import { applyPreset } from './previewPanel';
import { store } from './state';
import { toast } from './toast';

export function createSettingsDrawer(): Drawer {
  const st = store();
  let timer = 0;
  const drawer = createDrawer('Settings', () => render());

  function commit(fn: (s: AppSettings) => AppSettings): void {
    st.updateSettings(fn);
    window.clearTimeout(timer);
    timer = window.setTimeout(() => void persistSettings(), 500);
  }
  const setStyle = (patch: Partial<StyleSettings>): void => commit((s) => ({ ...s, style: { ...s.style, ...patch } }));
  const setName = (patch: Partial<FilenameRules>): void =>
    commit((s) => ({ ...s, filename: { ...s.filename, ...patch } }));
  const setExtract = (patch: Partial<ExtractOptions>): void =>
    commit((s) => ({ ...s, extract: { ...s.extract, ...patch } }));
  const setPack = (patch: Partial<PackLimits>): void =>
    commit((s) => ({ ...s, packLimits: { ...s.packLimits, ...patch } }));

  /** A number input that writes on input when the value is valid. */
  function num(value: number, min: number, max: number, step: number, write: (v: number) => void): HTMLInputElement {
    const el = h('input', { type: 'number', min, max, step, value: String(value) });
    el.addEventListener('input', () => {
      const v = Number(el.value);
      if (el.value !== '' && Number.isFinite(v) && v >= min && v <= max) write(v);
    });
    return el;
  }

  function render(): void {
    const s = st.get().settings;
    const y = s.style;
    const css = h('textarea', { rows: 8, spellcheck: false, class: 'code', value: y.customCss });
    css.addEventListener('input', () => setStyle({ customCss: css.value }));
    const helper = h('input', { type: 'url', placeholder: 'http://127.0.0.1:8134', value: s.helperUrl ?? '' });
    helper.addEventListener('input', () => commit((x) => ({ ...x, helperUrl: helper.value.trim() || undefined })));
    const tmpl = h('input', { type: 'text', value: s.filename.template, spellcheck: false });
    tmpl.addEventListener('input', () => setName({ template: tmpl.value }));
    const spaceRep = h('input', { type: 'text', value: s.filename.spaceReplacement, maxlength: 3, size: 3 });
    spaceRep.addEventListener('input', () => setName({ spaceReplacement: spaceRep.value }));

    const reExtract = h('button', {
      type: 'button',
      class: 'btn',
      onclick: () => {
        const input = st.get().input;
        if (!input) toast('No input to extract again. Load an article first.', 'warn');
        else void runExtract(input);
      },
    }, 'Run extraction again');

    const reset = h('button', {
      type: 'button',
      class: 'btn danger',
      onclick: async () => {
        if (!confirm('Reset all settings to the defaults?')) return;
        try {
          const fresh = await resetSettings();
          st.updateSettings(() => fresh);
          render();
          toast('Settings reset.', 'ok');
        } catch (err) {
          toast(`Reset failed: ${errorMessage(err)}`, 'error');
        }
      },
    }, 'Reset to defaults');

    replace(
      drawer.body,
      h(
        'fieldset',
        null,
        h('legend', null, 'Style'),
        field(
          'Preset',
          select(
            [['compact', 'Compact'], ['comfortable', 'Comfortable'], ['large', 'Large'], ['reference', 'Reference']],
            y.preset,
            (v) => {
              commit((x) => ({ ...x, style: applyPreset(x.style, v) }));
              render();
            },
          ),
          'A preset sets size, margin, line height and spacing.',
        ),
        field('Font size (%)', num(y.fontSizePercent, 50, 250, 5, (v) => setStyle({ fontSizePercent: v }))),
        field('Margin (em)', num(y.marginEm, 0, 4, 0.1, (v) => setStyle({ marginEm: v }))),
        field('Line height', num(y.lineHeight, 1, 2.5, 0.05, (v) => setStyle({ lineHeight: v }))),
        field('Paragraph spacing (em)', num(y.paragraphSpacingEm, 0, 3, 0.1, (v) => setStyle({ paragraphSpacingEm: v }))),
        checkbox('Hyphenation', y.hyphenate, (v) => setStyle({ hyphenate: v })),
        field(
          'Custom CSS (added to the EPUB stylesheet)',
          css,
          'CrossPoint ignores font-size and font-family. It reads margins, indent, alignment, weight and style.',
        ),
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Images'),
        field('Max image width (px)', num(y.imageMaxWidthPx, 64, 2000, 10, (v) => setStyle({ imageMaxWidthPx: v })), 'The X4 Pro screen is 480 px wide.'),
        field('JPEG quality (0.1 to 1)', num(y.imageQuality, 0.1, 1, 0.05, (v) => setStyle({ imageQuality: v }))),
        checkbox('Grayscale images', y.grayscaleImages, (v) => setStyle({ grayscaleImages: v })),
        checkbox('Include images in output', s.build.includeImages, (v) => commit((x) => ({ ...x, build: { ...x.build, includeImages: v } }))),
        checkbox('Cache image bytes in the library', s.cacheImages, (v) => commit((x) => ({ ...x, cacheImages: v }))),
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Links and tables'),
        checkbox('Keep links', y.keepLinks, (v) => setStyle({ keepLinks: v })),
        field(
          'Tables',
          select(
            [['keep', 'Keep tables'], ['linearize', 'One paragraph per row'], ['drop', 'Remove tables']],
            y.tableHandling,
            (v) => setStyle({ tableHandling: v }),
          ),
        ),
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Extraction'),
        checkbox('Keep links in the body', s.extract.keepLinks ?? true, (v) => setExtract({ keepLinks: v })),
        checkbox('Keep images in the body', s.extract.keepImages ?? true, (v) => setExtract({ keepImages: v })),
        checkbox('Keep tables in the body', s.extract.keepTables ?? true, (v) => setExtract({ keepTables: v })),
        checkbox('Wikipedia: keep infobox', s.extract.keepInfobox ?? false, (v) => setExtract({ keepInfobox: v })),
        checkbox('Wikipedia: keep references and notes', s.extract.keepReferences ?? false, (v) => setExtract({ keepReferences: v })),
        h('small', { class: 'hint' }, 'Extraction options apply to the next extraction.'),
        reExtract,
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Output'),
        checkbox('Include cover page', s.build.includeCover, (v) => commit((x) => ({ ...x, build: { ...x.build, includeCover: v } }))),
        checkbox('Include source note', s.build.includeSourceNote, (v) => commit((x) => ({ ...x, build: { ...x.build, includeSourceNote: v } }))),
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Filenames'),
        field('Template', tmpl, 'Fields: {title} {author} {date} {site}'),
        field('Max length', num(s.filename.maxLength, 10, 200, 1, (v) => setName({ maxLength: v }))),
        checkbox('Lowercase', s.filename.lowercase, (v) => setName({ lowercase: v })),
        field('Replace spaces with', spaceRep, 'Empty keeps spaces. Use - or _.'),
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Spider packs (defaults)'),
        field(
          'Depth',
          select<'0' | '1' | '2'>(
            [['0', '0 - root only'], ['1', '1 - root + its links'], ['2', '2 - links of links (slow)']],
            String(s.packLimits.depth) as '0' | '1' | '2',
            (v) => setPack({ depth: Number(v) as 0 | 1 | 2 }),
          ),
        ),
        field('Max articles (root included)', num(s.packLimits.maxArticles, 1, 500, 1, (v) => setPack({ maxArticles: v }))),
        field('Max images per article', num(s.packLimits.maxImagesPerArticle, 0, 50, 1, (v) => setPack({ maxImagesPerArticle: v }))),
        field('Max EPUB size (MB)', num(s.packLimits.maxTotalEpubSizeMB, 1, 500, 1, (v) => setPack({ maxTotalEpubSizeMB: v }))),
        field('Delay between requests (ms)', num(s.packLimits.requestDelayMs, 0, 10000, 50, (v) => setPack({ requestDelayMs: v })), 'Be polite to Wikipedia. 500 ms is a good value.'),
        checkbox('Include references', s.packLimits.includeReferences, (v) => setPack({ includeReferences: v })),
        checkbox('Include infobox', s.packLimits.includeInfobox, (v) => setPack({ includeInfobox: v })),
        checkbox('Web packs: same site only', s.packLimits.sameDomainOnly, (v) => setPack({ sameDomainOnly: v })),
      ),
      h(
        'fieldset',
        null,
        h('legend', null, 'Local helper app'),
        field('Helper URL', helper, 'Optional. The helper forwards files to the device from http://127.0.0.1. See helper/README.md.'),
      ),
      h('div', { class: 'btn-row' }, reset),
    );
  }

  // Re-render when settings change from outside (preset select), but not while the user types here.
  st.subscribe(() => {
    if (drawer.isOpen() && !drawer.el.contains(document.activeElement)) render();
  }, ['settings']);

  return drawer;
}
