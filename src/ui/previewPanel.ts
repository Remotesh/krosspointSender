/**
 * Preview panel (center column). It shows the cleaned article inside a
 * 480x800 device frame. It uses the same CSS as the EPUB, scoped to .paper.
 */
import type { StylePreset, StyleSettings } from '../types';
import { epubCss, PRESETS } from '../epub';
import { estimateReading } from '../converters';
import { h, replace, select } from './dom';
import { persistSettings } from './actions';
import { store, viewArticle } from './state';
import { mountPackList } from './packList';
import { packStats } from './packOutput';

/**
 * Prefix every selector in a stylesheet with a scope. "html" and "body"
 * become the scope itself. @media and @supports blocks are kept and their
 * inner rules are prefixed. Other at-rules (@page, @font-face) are dropped.
 */
export function scopeCss(css: string, scope: string): string {
  const src = css.replace(/\/\*[\s\S]*?\*\//g, '');
  let out = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open < 0) break;
    const head = src.slice(i, open).trim();
    // Find the matching close brace.
    let depth = 1;
    let j = open + 1;
    while (j < src.length && depth > 0) {
      if (src[j] === '{') depth++;
      else if (src[j] === '}') depth--;
      j++;
    }
    const body = src.slice(open + 1, j - 1);
    if (head.startsWith('@media') || head.startsWith('@supports')) {
      out += `${head}{${scopeCss(body, scope)}}\n`;
    } else if (!head.startsWith('@') && head) {
      const sel = head
        .split(',')
        .map((s) => s.trim())
        .filter(Boolean)
        .map((s) => {
          if (/^(html|body)$/i.test(s)) return scope;
          if (/^(html|body)\b/i.test(s)) return s.replace(/^(html|body)/i, scope);
          return `${scope} ${s}`;
        })
        .join(', ');
      out += `${sel}{${body}}\n`;
    }
    i = j;
  }
  return out;
}

const ZOOMS: Array<[string, string]> = [
  ['fit', 'Fit'],
  ['0.5', '50%'],
  ['0.75', '75%'],
  ['1', '100%'],
  ['1.25', '125%'],
];

const PRESET_NAMES: Array<[StylePreset, string]> = [
  ['compact', 'Compact'],
  ['comfortable', 'Comfortable'],
  ['large', 'Large'],
  ['reference', 'Reference'],
];

/** Apply a preset. Keep the user's image, quality and custom CSS choices. */
export function applyPreset(style: StyleSettings, preset: StylePreset): StyleSettings {
  const p = PRESETS[preset];
  return {
    ...p,
    preset,
    imageMaxWidthPx: style.imageMaxWidthPx,
    grayscaleImages: style.grayscaleImages,
    imageQuality: style.imageQuality,
    hyphenate: style.hyphenate,
    customCss: style.customCss,
  };
}

export function mountPreviewPanel(root: HTMLElement): void {
  const st = store();
  const styleEl = h('style');
  const paper = h('div', { class: 'paper', lang: 'en' });
  const screen = h('div', { class: 'device-screen', tabindex: 0, 'aria-label': 'Article preview' }, paper);
  const frame = h('div', { class: 'device-frame' }, screen, h('div', { class: 'device-chin' }, 'X4 Pro'));
  const stage = h('div', { class: 'device-stage' }, frame);
  const stats = h('div', { class: 'preview-stats', 'aria-live': 'polite' });

  let zoomMode = 'fit';
  const imgToggle = h('input', { type: 'checkbox', checked: st.get().showImages });
  imgToggle.addEventListener('change', () => st.set({ showImages: imgToggle.checked }));
  const presetSel = select(PRESET_NAMES, st.get().settings.style.preset, (v) => {
    st.updateSettings((s) => ({ ...s, style: applyPreset(s.style, v) }));
    void persistSettings();
  });
  const zoomSel = select(ZOOMS, zoomMode, (v) => {
    zoomMode = v;
    applyZoom();
  });
  presetSel.id = 'preview-preset';
  zoomSel.id = 'preview-zoom';

  const toolbar = h(
    'div',
    { class: 'preview-tools' },
    h('label', { class: 'check' }, imgToggle, ' Images'),
    h('label', { for: 'preview-preset' }, 'Preset ', presetSel),
    h('label', { for: 'preview-zoom' }, 'Zoom ', zoomSel),
  );

  replace(root, h('h2', null, 'Preview'), mountPackList(), toolbar, stage, stats, styleEl);

  /** Scale the frame. "fit" uses the stage height and width. */
  function applyZoom(): void {
    let z = Number(zoomMode);
    if (zoomMode === 'fit') {
      const w = stage.clientWidth - 16;
      const hgt = Math.max(320, window.innerHeight - stage.getBoundingClientRect().top - 90);
      // Frame is 480 + 2*24 bezel wide, 800 + 24 + 40 tall.
      z = Math.max(0.3, Math.min(1.25, w / 528, hgt / 864));
    }
    frame.style.setProperty('--zoom', String(z));
    stage.style.height = `${Math.ceil(864 * z) + 8}px`;
  }
  window.addEventListener('resize', applyZoom);
  new ResizeObserver(applyZoom).observe(stage);

  function renderStyle(): void {
    const s = st.get().settings.style;
    styleEl.textContent = scopeCss(epubCss(s), '.paper');
    paper.classList.toggle('gray-images', s.grayscaleImages);
    paper.classList.toggle('hyphenate', s.hyphenate);
    presetSel.value = s.preset;
  }

  let lastId = '';
  function renderBody(): void {
    const a = viewArticle(st.get());
    if (!a && st.get().mode === 'pack') {
      replace(
        paper,
        h(
          'div',
          { class: 'empty' },
          h('h1', null, 'Pack: no page fetched yet'),
          h('p', null, 'Select pages and build the pack in the [ pack ] tab. Then click a page in the list to read it here.'),
        ),
      );
      return;
    }
    if (!a) {
      replace(
        paper,
        h(
          'div',
          { class: 'empty' },
          h('h1', null, 'No article yet'),
          h('p', null, 'Fetch a URL, paste HTML or text, or drop a file on the left panel.'),
          h('p', null, 'The preview uses the same stylesheet as the EPUB.'),
        ),
      );
      return;
    }
    paper.lang = a.language || 'en';
    const keepScroll = a.id === lastId ? screen.scrollTop : 0;
    lastId = a.id;
    // bodyHtml is sanitized by the extractor (allowlist).
    paper.innerHTML = a.bodyHtml;
    // The EPUB shows the title on the cover or on top of the first chapter. Do the same here.
    paper.prepend(
      h('h1', { class: 'article-title' }, a.title),
      a.author ? h('p', { class: 'article-byline' }, a.author) : '',
    );
    for (const img of Array.from(paper.querySelectorAll('img'))) {
      img.loading = 'lazy';
      img.referrerPolicy = 'no-referrer';
      img.decoding = 'async';
    }
    screen.scrollTop = keepScroll;
  }

  function renderStats(): void {
    const s = st.get();
    if (s.mode === 'pack' && s.pack) {
      const p = packStats(s.pack);
      const est = s.packOutput?.file.estimatedPages ?? p.pages;
      replace(
        stats,
        h('span', null, `${p.articles} articles`),
        h('span', null, `~${est} pages on X4 Pro`),
        h('span', null, `${p.words.toLocaleString()} words`),
        h('span', null, `${Math.ceil(p.words / 230)} min read`),
        h('span', null, `${s.packOutput ? s.packOutput.images.size : p.images} images`),
        p.failed ? h('span', { class: 'bad' }, `${p.failed} failed`) : null,
      );
      return;
    }
    const { article, settings } = s;
    if (!article) {
      stats.textContent = '';
      return;
    }
    const est = estimateReading(article, settings.style);
    replace(
      stats,
      h('span', null, `${est.minutes} min read`),
      h('span', null, `~${est.pages} pages on X4 Pro`),
      h('span', null, `${article.wordCount.toLocaleString()} words`),
      h('span', null, `${article.images.length} images`),
      h('span', null, `${article.toc.length} headings`),
    );
  }

  // Links in the preview do not navigate. Anchors scroll inside the screen.
  paper.addEventListener('click', (e) => {
    const a = (e.target as Element).closest('a');
    if (!a) return;
    e.preventDefault();
    const href = a.getAttribute('href') ?? '';
    if (href.startsWith('#')) {
      paper.querySelector(`[id="${CSS.escape(href.slice(1))}"]`)?.scrollIntoView({ block: 'start' });
      return;
    }
    // Pack mode: a link to a fetched page of the pack opens that page.
    const s = st.get();
    if (s.mode === 'pack' && s.pack) {
      const target = href.split('#')[0];
      const same = (u: string): boolean => {
        try {
          return decodeURIComponent(u).replace(/_/g, ' ') === decodeURIComponent(target).replace(/_/g, ' ');
        } catch {
          return u === target;
        }
      };
      const page = s.pack.pages.find((p) => p.article && same(p.candidate.url));
      if (page) st.set({ packPreviewKey: page.candidate.key });
    }
  });

  st.subscribe(() => {
    renderBody();
    renderStats();
  }, ['article', 'mode', 'pack', 'packPreviewKey', 'packOutput']);
  st.subscribe(() => {
    renderStyle();
    renderStats();
  }, ['settings']);
  st.subscribe((s) => {
    paper.classList.toggle('no-images', !s.showImages);
    imgToggle.checked = s.showImages;
  }, ['showImages']);

  renderStyle();
  renderBody();
  renderStats();
  paper.classList.toggle('no-images', !st.get().showImages);
  requestAnimationFrame(applyZoom);
}
