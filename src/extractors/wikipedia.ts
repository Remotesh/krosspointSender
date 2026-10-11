/**
 * Wikipedia clean-up. It works on the desktop page and on Parsoid HTML.
 */
import type { Article, ExtractOptions } from '../types';
import { normText, parseDocument, unwrap } from './dom';
import { sanitizeHtml } from './sanitize';
import { finishArticle } from './finish';

/** Selectors that we always remove. */
const ALWAYS_REMOVE = [
  '.navbox', '.vertical-navbox', '.sidebar', '.mw-editsection', '.hatnote', '.ambox', '#toc', '.toc', '.metadata',
  '.catlinks', '.mw-jump-link', '.noprint', '.portal', '#coordinates', '.mw-empty-elt', '.shortdescription',
  '.navbox-styles', '.mw-authority-control', '.authority-control', '.sistersitebox', '.side-box', '.mbox-small',
  '.mw-indicators', '.printfooter', '.mw-cite-backlink', 'style', 'link', 'meta', 'script', 'noscript',
];

/** Reference markers and lists. keepReferences keeps them. */
const REFERENCE_SELECTORS = ['.reference', 'sup.reference', '.mw-references-wrap', 'ol.references', '.reflist'];

/** Section titles we remove. The first group stays with keepReferences. */
const REF_SECTIONS = ['references', 'notes', 'citations', 'notes and references', 'references and notes', 'footnotes', 'sources', 'works cited'];
const OTHER_SECTIONS = ['external links', 'see also', 'further reading', 'bibliography'];

function headingLevel(n: Node | null): number {
  if (!n || n.nodeType !== 1) return 0;
  const el = n as Element;
  if (/^H[1-6]$/.test(el.tagName)) return Number(el.tagName[1]);
  if (el.classList.contains('mw-heading')) {
    const h = el.querySelector('h1, h2, h3, h4, h5, h6');
    return h ? Number(h.tagName[1]) : 0;
  }
  if (el.tagName === 'SECTION') {
    const first = el.firstElementChild;
    return first ? headingLevel(first) : 0;
  }
  return 0;
}

function headingTitle(h: Element): string {
  const clone = h.cloneNode(true) as Element;
  clone.querySelectorAll('.mw-editsection').forEach((e) => e.remove());
  return normText(clone.textContent).replace(/\[\s*edit\s*\]/gi, '').trim().toLowerCase();
}

function removeSections(root: Element, titles: Set<string>): void {
  for (const h of Array.from(root.querySelectorAll('h2, h3, h4, h5, h6'))) {
    if (!h.isConnected || !titles.has(headingTitle(h))) continue;
    const level = Number(h.tagName[1]);
    const unit = h.parentElement?.classList.contains('mw-heading') ? h.parentElement : h;
    const sec = unit.parentElement;
    if (sec && sec.tagName === 'SECTION' && sec.firstElementChild === unit) {
      sec.remove();
      continue;
    }
    let n = unit.nextSibling;
    unit.remove();
    while (n) {
      const lvl = headingLevel(n);
      if (lvl && lvl <= level) break;
      const next = n.nextSibling;
      n.parentNode?.removeChild(n);
      n = next;
    }
  }
}

function absImageSrc(img: Element, base: string): string | null {
  const last = (img.getAttribute('srcset') ?? '')
    .split(',')
    .map((s) => s.trim().split(/\s+/)[0])
    .filter(Boolean)
    .pop();
  const raw = last || img.getAttribute('src') || img.getAttribute('data-src');
  if (!raw) return null;
  try {
    const u = new URL(raw.startsWith('//') ? 'https:' + raw : raw, base);
    if (u.protocol === 'http:') u.protocol = 'https:';
    return u.toString();
  } catch {
    return null;
  }
}

/** Turn .thumb blocks, figure[typeof] and gallery boxes into figure/img/figcaption. */
function convertFigures(root: Element, base: string): void {
  const doc = root.ownerDocument;
  // Lazy image placeholders on the mobile site.
  for (const ph of Array.from(root.querySelectorAll('span.lazy-image-placeholder[data-src]'))) {
    const img = doc.createElement('img');
    img.setAttribute('src', ph.getAttribute('data-src')!);
    const ss = ph.getAttribute('data-srcset');
    if (ss) img.setAttribute('srcset', ss);
    img.setAttribute('alt', ph.getAttribute('data-alt') ?? '');
    ph.replaceWith(img);
  }
  for (const img of Array.from(root.querySelectorAll('img'))) {
    const src = absImageSrc(img, base);
    if (!src) {
      img.remove();
      continue;
    }
    img.setAttribute('src', src);
    img.removeAttribute('srcset');
    img.removeAttribute('data-src');
    // Remove small icons (flags, edit icons).
    const w = Number(img.getAttribute('width'));
    if (w > 0 && w < 24) img.remove();
  }
  const blocks = Array.from(root.querySelectorAll('.thumb, figure[typeof], figure[class*="mw-"], li.gallerybox, .tsingle'));
  for (const block of blocks) {
    if (!block.isConnected) continue;
    const imgs = Array.from(block.querySelectorAll('img'));
    if (!imgs.length) {
      block.remove();
      continue;
    }
    const capSrc = block.querySelector('figcaption, .thumbcaption, .gallerytext, .thumbcaption-inner');
    const fig = doc.createElement('figure');
    for (const i of imgs) {
      const ni = doc.createElement('img');
      ni.setAttribute('src', i.getAttribute('src')!);
      ni.setAttribute('alt', i.getAttribute('alt') ?? '');
      fig.appendChild(ni);
    }
    if (capSrc) {
      const capClone = capSrc.cloneNode(true) as Element;
      capClone.querySelectorAll('.magnify, sup.reference, .reference').forEach((e) => e.remove());
      if (normText(capClone.textContent)) {
        const cap = doc.createElement('figcaption');
        cap.innerHTML = capClone.innerHTML;
        cap.querySelectorAll('div, p').forEach((e) => unwrap(e));
        fig.appendChild(cap);
      }
    }
    if (block.tagName === 'LI') {
      // A gallery is a list. Move the figure out of the list.
      const gallery = block.closest('ul, ol');
      if (gallery) {
        gallery.before(fig);
        block.remove();
        if (!gallery.querySelector('li')) gallery.remove();
        continue;
      }
    }
    block.replaceWith(fig);
  }
  // Math: keep the TeX text from the fallback image.
  for (const m of Array.from(root.querySelectorAll('.mwe-math-element'))) {
    const alt = m.querySelector('img')?.getAttribute('alt') ?? m.querySelector('annotation')?.textContent ?? '';
    const code = doc.createElement('code');
    code.textContent = normText(alt);
    m.replaceWith(code);
  }
}

/** Extract a Wikipedia article. `url` resolves relative links. */
export function extractWikipedia(html: string, url: string, opts: ExtractOptions = {}): Article {
  const base = url || 'https://en.wikipedia.org/wiki/';
  const doc = parseDocument(html, base);
  const title =
    normText(doc.querySelector('#firstHeading')?.textContent) ||
    normText(doc.title).replace(/\s+[-–—]\s+Wikipedia.*$/i, '') ||
    normText(doc.querySelector('h1')?.textContent) ||
    'Untitled';
  const htmlLang = doc.documentElement.getAttribute('lang') || doc.querySelector('#mw-content-text')?.getAttribute('lang');
  let hostLang: string | undefined;
  try {
    const sub = new URL(base).hostname.split('.')[0];
    if (/^[a-z]{2,3}(-[a-z]+)?$/.test(sub) && sub !== 'www') hostLang = sub;
  } catch {
    /* No language from the host. */
  }

  const root =
    doc.querySelector('#mw-content-text .mw-parser-output') ??
    doc.querySelector('#mw-content-text') ??
    doc.querySelector('.mw-parser-output') ??
    doc.body;

  const remove = [...ALWAYS_REMOVE];
  if (!opts.keepInfobox) remove.push('.infobox', '.infobox_v2', '.infobox_v3');
  if (!opts.keepReferences) remove.push(...REFERENCE_SELECTORS);
  for (const el of Array.from(root.querySelectorAll(remove.join(',')))) el.remove();

  const titles = new Set<string>(OTHER_SECTIONS);
  if (!opts.keepReferences) REF_SECTIONS.forEach((t) => titles.add(t));
  else ['sources', 'works cited'].forEach((t) => titles.add(t));
  removeSections(root, titles);

  // Remove leftover "[edit]" text.
  for (const h of Array.from(root.querySelectorAll('h1, h2, h3, h4, h5, h6'))) {
    for (const t of Array.from(h.childNodes)) {
      if (t.nodeType === 3 && /\[\s*edit\s*\]/i.test(t.textContent ?? '')) t.textContent = (t.textContent ?? '').replace(/\[\s*edit\s*\]/gi, '');
    }
  }
  convertFigures(root, base);

  const firstPara = Array.from(root.querySelectorAll('p')).map((p) => normText(p.textContent)).find((t) => t.length > 40);
  const bodyHtml = sanitizeHtml(root.innerHTML, {
    baseUrl: base,
    keepLinks: opts.keepLinks ?? true,
    keepImages: opts.keepImages ?? true,
    keepTables: opts.keepTables ?? true,
  });

  return finishArticle({
    bodyHtml,
    inputKind: 'wikipedia',
    title,
    author: 'Wikipedia contributors',
    siteName: 'Wikipedia',
    sourceUrl: url || undefined,
    language: htmlLang || hostLang || 'en',
    excerpt: firstPara ? (firstPara.length > 280 ? firstPara.slice(0, 277).trimEnd() + '...' : firstPara) : undefined,
  });
}
