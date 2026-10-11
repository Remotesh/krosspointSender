/**
 * DOM helpers shared by the extractors.
 * All functions change the given tree in place.
 */

/** Parse a full HTML document. Adds a <base> when baseUrl is set and the page has none. */
export function parseDocument(html: string, baseUrl?: string): Document {
  const doc = new DOMParser().parseFromString(html, 'text/html');
  if (baseUrl && !doc.querySelector('base[href]')) {
    try {
      const base = doc.createElement('base');
      base.setAttribute('href', new URL(baseUrl).toString());
      doc.head.insertBefore(base, doc.head.firstChild);
    } catch {
      /* A bad baseUrl is not fatal. */
    }
  }
  return doc;
}

/** Parse an HTML fragment. The content is in `doc.body`. */
export function parseFragment(html: string): Document {
  return new DOMParser().parseFromString(`<!DOCTYPE html><html><head></head><body>${html}</body></html>`, 'text/html');
}

/** Collapse whitespace and trim. */
export function normText(s: string | null | undefined): string {
  return (s ?? '').replace(/\s+/g, ' ').trim();
}

/** Replace an element with its children. */
export function unwrap(el: Element): void {
  const parent = el.parentNode;
  if (!parent) return;
  while (el.firstChild) parent.insertBefore(el.firstChild, el);
  parent.removeChild(el);
}

/** Make a new element with the given tag and move the children of `el` into it. */
export function renameElement(el: Element, tag: string): Element {
  const doc = el.ownerDocument;
  const out = doc.createElement(tag);
  for (const name of ['id', 'colspan', 'rowspan']) {
    const v = el.getAttribute(name);
    if (v != null) out.setAttribute(name, v);
  }
  while (el.firstChild) out.appendChild(el.firstChild);
  el.replaceWith(out);
  return out;
}

// ───────────── Junk removal ─────────────

/** Tags that never carry article text. We remove them with their content. */
export const JUNK_TAGS = [
  'script', 'style', 'link', 'meta', 'template', 'noscript', 'iframe', 'frame', 'frameset', 'object', 'embed',
  'form', 'button', 'input', 'select', 'textarea', 'label', 'dialog', 'canvas', 'svg', 'video', 'audio',
  'nav', 'header', 'footer', 'aside', 'menu',
];

/** Roles that mark page chrome, not content. */
export const JUNK_ROLES = ['navigation', 'banner', 'contentinfo', 'complementary', 'search', 'dialog', 'alertdialog', 'menu', 'menubar', 'toolbar'];

/**
 * Class and id tokens that mark junk.
 * A token matches when the word is the whole token or a part between "-" or "_".
 * Example: "share" matches "share-bar" and "post_share", not "shared".
 */
const JUNK_WORDS = [
  // ads and sponsors
  'ad', 'ads', 'adv', 'advert', 'adverts', 'advertisement', 'advertising', 'adsbygoogle', 'adslot', 'adunit', 'adbox',
  'adcontainer', 'adwrapper', 'dfp', 'gpt', 'sponsor', 'sponsored', 'sponsors', 'promo', 'promoted', 'promotion',
  'outbrain', 'taboola', 'revcontent', 'zergnet', 'mgid', 'teads', 'affiliate',
  // cookie and consent
  'cookie', 'cookies', 'consent', 'gdpr', 'cmp', 'onetrust', 'cookiebanner', 'cookiebar', 'didomi', 'qc', 'privacy',
  // comments
  'comment', 'comments', 'commentlist', 'disqus', 'respond', 'replies', 'discussion', 'coral',
  // related and recommended
  'related', 'recommended', 'recommendations', 'more', 'readmore', 'read', 'popular', 'trending', 'most', 'next', 'prev',
  'also', 'upnext',
  // share and social
  'share', 'sharing', 'sharebar', 'sharetools', 'social', 'socials', 'sociallinks', 'follow', 'addthis', 'sharethis',
  // navigation and chrome
  'nav', 'navbar', 'navigation', 'menu', 'breadcrumb', 'breadcrumbs', 'sidebar', 'masthead', 'topbar', 'toolbar',
  'skip', 'skiplink', 'pagination', 'pager', 'header', 'footer', 'site', 'sitefooter', 'siteheader',
  // popups and forms
  'newsletter', 'subscribe', 'subscription', 'signup', 'paywall', 'regwall', 'popup', 'modal', 'overlay', 'banner',
  'lightbox', 'toast', 'login', 'register',
  // misc widgets
  'widget', 'print', 'hidden', 'screenreader', 'sr', 'visually', 'author-bio', 'tags', 'tagcloud',
];

/** Some words are too weak alone. They match only as a combined token like "read-more". */
const WEAK_WORDS = new Set(['more', 'read', 'most', 'next', 'prev', 'also', 'site', 'print', 'hidden', 'sr', 'visually', 'qc', 'privacy', 'skip', 'follow', 'login', 'register', 'tags']);
const WEAK_COMBOS = [
  /^read[-_]?more/, /more[-_](stories|articles|from|news|like)/, /most[-_](read|popular|viewed)/, /(^|[-_])also[-_]read/,
  /^(next|prev)[-_](post|article|story)/, /^site[-_]?(header|footer|nav|menu)/, /^print[-_]?(only|button)/,
  /^(sr|screen[-_]?reader|visually)[-_]?(only|hidden)/, /^skip[-_]?(link|to)/, /^follow[-_]?(us|button)/,
  /^(login|register)[-_]?(form|wall|prompt)/, /^tags?[-_]?(list|cloud|links)/, /privacy[-_]?(banner|notice|popup)/,
];

const STRONG = JUNK_WORDS.filter((w) => !WEAK_WORDS.has(w));
const STRONG_RE = new RegExp(`(^|[-_])(${STRONG.map((w) => w.replace(/[-]/g, '[-_]?')).join('|')})([-_]|$)`, 'i');

/** True when a class token or id looks like page junk. */
export function isJunkToken(token: string): boolean {
  const t = token.toLowerCase();
  if (!t) return false;
  // Words that look like junk but carry content.
  if (/^(article|entry|post|story|content|main|body|text)$/.test(t)) return false;
  if (/(^|[-_])(header|heading|headline|title)([-_]|$)/.test(t) && /(article|entry|post|story|content|page)[-_]?(header|heading)/.test(t)) return false;
  if (STRONG_RE.test(t)) return true;
  return WEAK_COMBOS.some((re) => re.test(t));
}

/** Elements we never remove by class or id. */
const PROTECTED = new Set(['HTML', 'BODY', 'MAIN', 'ARTICLE']);

/**
 * Remove page chrome: junk tags, junk roles, hidden nodes, and elements with junk class or id tokens.
 * An element that holds most of the page text, or holds the <article>/<main>, stays.
 */
export function removeJunk(root: ParentNode, opts: { patterns?: boolean } = {}): void {
  const patterns = opts.patterns ?? true;
  for (const el of Array.from(root.querySelectorAll(JUNK_TAGS.join(',')))) el.remove();
  for (const el of Array.from(root.querySelectorAll(JUNK_ROLES.map((r) => `[role="${r}"]`).join(',')))) el.remove();
  for (const el of Array.from(root.querySelectorAll('[hidden], [aria-hidden="true"]:not(img), [style*="display:none"], [style*="display: none"]'))) {
    el.remove();
  }
  // Remove HTML comments.
  const doc = (root as Node).ownerDocument ?? (root as unknown as Document);
  const walker = doc.createTreeWalker(root as Node, 128 /* NodeFilter.SHOW_COMMENT */);
  const comments: Node[] = [];
  while (walker.nextNode()) comments.push(walker.currentNode);
  for (const c of comments) c.parentNode?.removeChild(c);

  if (!patterns) return;
  const bodyEl = (root as Document).body ?? (root as Element);
  const totalText = normText(bodyEl.textContent).length || 1;
  const candidates = Array.from(root.querySelectorAll('[class], [id]'));
  for (const el of candidates) {
    if (!el.isConnected || PROTECTED.has(el.tagName)) continue;
    const tokens = [...Array.from(el.classList), el.id].filter(Boolean);
    if (!tokens.some(isJunkToken)) continue;
    if (el.querySelector('article, main, [itemprop="articleBody"]')) continue;
    if (/^H[1-6]$/.test(el.tagName)) continue;
    const len = normText(el.textContent).length;
    if (len > totalText * 0.4 && len > 500) continue;
    el.remove();
  }
}

// ───────────── Images and figures ─────────────

/** Pick the largest candidate from a srcset. Falls back to the last entry. */
export function pickFromSrcset(srcset: string | null, preferLast = false): string | null {
  if (!srcset) return null;
  const entries = srcset
    .split(/,\s+(?=\S)/)
    .map((s) => s.trim())
    .filter(Boolean)
    .map((s) => {
      const [url, desc] = s.split(/\s+/);
      const m = /^([\d.]+)([wx])$/.exec(desc ?? '');
      return { url, size: m ? Number(m[1]) * (m[2] === 'x' ? 1000 : 1) : 0 };
    })
    .filter((e) => e.url);
  if (!entries.length) return null;
  if (preferLast) return entries[entries.length - 1].url;
  let best = entries[entries.length - 1];
  for (const e of entries) if (e.size > best.size) best = e;
  return best.url;
}

function isPlaceholderSrc(src: string | null): boolean {
  if (!src) return true;
  const s = src.trim();
  if (!s) return true;
  if (/^data:image\/(gif|svg)/i.test(s) && s.length < 400) return true;
  return /(blank|spacer|placeholder|transparent|pixel|lazy)[^/]*\.(gif|png|svg)/i.test(s);
}

const LAZY_ATTRS = ['data-src', 'data-lazy-src', 'data-original', 'data-orig-file', 'data-url', 'data-hi-res-src', 'data-full-src', 'data-lazyload'];

/** Give every <img> a real src: lazy attributes, srcset, <picture> sources. Drop images with none. */
export function fixImages(root: ParentNode, opts: { preferSrcset?: boolean } = {}): void {
  for (const pic of Array.from(root.querySelectorAll('picture'))) {
    const img = pic.querySelector('img');
    if (!img) {
      pic.remove();
      continue;
    }
    if (isPlaceholderSrc(img.getAttribute('src'))) {
      const source = Array.from(pic.querySelectorAll('source')).find((s) => !/webp|avif/i.test(s.getAttribute('type') ?? ''))
        ?? pic.querySelector('source');
      const cand = pickFromSrcset(source?.getAttribute('srcset') ?? source?.getAttribute('data-srcset') ?? null);
      if (cand) img.setAttribute('src', cand);
    }
    pic.replaceWith(img);
  }
  for (const img of Array.from(root.querySelectorAll('img'))) {
    let src = img.getAttribute('src');
    if (isPlaceholderSrc(src)) {
      const lazy = LAZY_ATTRS.map((a) => img.getAttribute(a)).find((v) => v && !isPlaceholderSrc(v));
      src = lazy ?? pickFromSrcset(img.getAttribute('data-srcset') ?? img.getAttribute('srcset')) ?? src;
    } else if (opts.preferSrcset) {
      src = pickFromSrcset(img.getAttribute('srcset'), true) ?? src;
    }
    if (!src || isPlaceholderSrc(src)) {
      img.remove();
      continue;
    }
    img.setAttribute('src', src);
    img.removeAttribute('srcset');
    img.removeAttribute('data-srcset');
    // Tracking pixels.
    const w = Number(img.getAttribute('width'));
    const h = Number(img.getAttribute('height'));
    if ((w > 0 && w <= 2) || (h > 0 && h <= 2)) img.remove();
  }
}

function buildFigure(doc: Document, imgs: Element[], captionNode: Element | null): Element {
  const fig = doc.createElement('figure');
  for (const img of imgs) {
    const clean = doc.createElement('img');
    clean.setAttribute('src', img.getAttribute('src') ?? '');
    const alt = img.getAttribute('alt');
    clean.setAttribute('alt', alt ?? '');
    fig.appendChild(clean);
  }
  if (captionNode && normText(captionNode.textContent)) {
    const cap = doc.createElement('figcaption');
    cap.innerHTML = captionNode.innerHTML;
    // A caption holds inline content only.
    for (const b of Array.from(cap.querySelectorAll('div, p, figure, img'))) {
      if (b.tagName === 'IMG' || b.tagName === 'FIGURE') b.remove();
      else unwrap(b);
    }
    fig.appendChild(cap);
  }
  return fig;
}

/**
 * Rebuild figure layouts as <figure><img><figcaption></figure>.
 * Handles <figure>, WordPress captions, and wrappers with one image and a caption class.
 */
export function normalizeFigures(root: ParentNode): void {
  const doc = (root as Node).ownerDocument ?? (root as unknown as Document);
  for (const fig of Array.from(root.querySelectorAll('figure'))) {
    if (!fig.isConnected) continue;
    const imgs = Array.from(fig.querySelectorAll('img'));
    if (!imgs.length) {
      // A figure without an image (quote, code). Keep the content.
      if (!normText(fig.textContent)) fig.remove();
      else if (fig.querySelector('pre, blockquote, table')) unwrap(fig);
      continue;
    }
    const cap = fig.querySelector('figcaption') ?? fig.querySelector('[class*="caption"]');
    fig.replaceWith(buildFigure(doc, imgs, cap));
  }
  const wrappers = Array.from(root.querySelectorAll('div, span, p, section'));
  for (const el of wrappers) {
    if (!el.isConnected) continue;
    const imgs = el.querySelectorAll('img');
    if (imgs.length !== 1) continue;
    const cap = el.querySelector('figcaption, [class*="caption"], [class*="Caption"], .credit');
    if (!cap || cap === el) continue;
    // The wrapper must hold only the image, the caption, and little else.
    const capLen = normText(cap.textContent).length;
    const restLen = normText(el.textContent).length - capLen;
    if (restLen > 20 || el.querySelector('p ~ p, h1, h2, h3, h4, h5, h6, ul, ol, table')) continue;
    el.replaceWith(buildFigure(doc, [imgs[0]], cap));
  }
}

// ───────────── Linearize ─────────────

const BLOCK_TAGS = new Set([
  'P', 'H1', 'H2', 'H3', 'H4', 'H5', 'H6', 'UL', 'OL', 'LI', 'DL', 'DT', 'DD', 'BLOCKQUOTE', 'PRE', 'TABLE', 'FIGURE',
  'FIGCAPTION', 'HR', 'DIV', 'SECTION', 'ARTICLE', 'MAIN', 'HEADER', 'FOOTER', 'ASIDE', 'NAV', 'ADDRESS', 'DETAILS',
  'SUMMARY', 'FIELDSET', 'HGROUP', 'CENTER', 'THEAD', 'TBODY', 'TFOOT', 'TR', 'TD', 'TH', 'CAPTION', 'FORM',
]);
const CONTAINERS = ['div', 'section', 'article', 'main', 'center', 'details', 'summary', 'address', 'fieldset', 'hgroup', 'header', 'footer', 'span[data-block]'];

export function isBlock(n: Node): boolean {
  return n.nodeType === 1 && BLOCK_TAGS.has((n as Element).tagName);
}

function hasContent(nodes: Node[]): boolean {
  return nodes.some((n) => (n.nodeType === 3 ? /\S/.test(n.textContent ?? '') : n.nodeType === 1 && ((n as Element).tagName === 'IMG' || !!(n as Element).querySelector?.('img') || /\S/.test(n.textContent ?? ''))));
}

/** Wrap each run of inline children of `parent` in a <p>. Whitespace-only runs are dropped. */
export function wrapInlineRuns(parent: Element): void {
  const doc = parent.ownerDocument;
  let run: Node[] = [];
  const flush = (before: Node | null) => {
    if (run.length && hasContent(run)) {
      const p = doc.createElement('p');
      parent.insertBefore(p, before);
      for (const n of run) p.appendChild(n);
    } else {
      for (const n of run) if (n.nodeType === 3) n.parentNode?.removeChild(n);
    }
    run = [];
  };
  for (const child of Array.from(parent.childNodes)) {
    if (isBlock(child)) flush(child);
    else if (child.nodeType === 1 && (child as Element).tagName === 'BR' && run.length === 0) parent.removeChild(child);
    else run.push(child);
  }
  flush(null);
}

/**
 * Make the tree linear.
 * A container with only inline content becomes a <p>.
 * A container with block content is unwrapped. Its loose inline runs become <p>.
 */
export function linearize(root: ParentNode): void {
  const list = Array.from(root.querySelectorAll(CONTAINERS.join(','))).reverse();
  for (const el of list) {
    if (!el.isConnected) continue;
    const kids = Array.from(el.childNodes);
    if (kids.some(isBlock)) {
      wrapInlineRuns(el);
      unwrap(el);
    } else if (hasContent(kids)) {
      const parent = el.parentElement;
      // Inside a <p>, <li>, cell or caption a <p> is not needed.
      if (parent && /^(P|LI|TD|TH|DT|DD|FIGCAPTION|CAPTION|H[1-6]|A|SPAN|B|STRONG|I|EM)$/.test(parent.tagName)) unwrap(el);
      else renameElement(el, 'p');
    } else {
      el.remove();
    }
  }
}

// ───────────── Tables ─────────────

/** Unwrap layout tables. When keepTables is false, turn each row into a paragraph. */
export function handleTables(root: ParentNode, keepTables = true): void {
  const doc = (root as Node).ownerDocument ?? (root as unknown as Document);
  // Layout tables: a table that holds another table, or a single cell.
  let guard = 0;
  for (;;) {
    const outer = Array.from(root.querySelectorAll('table')).find(
      (t) => t.querySelector('table') || t.querySelectorAll('td, th').length <= 1,
    );
    if (!outer || guard++ > 200) break;
    const box = doc.createElement('div');
    for (const cap of Array.from(outer.querySelectorAll(':scope > caption'))) {
      const p = doc.createElement('p');
      p.innerHTML = cap.innerHTML;
      box.appendChild(p);
    }
    for (const cell of Array.from(outer.querySelectorAll('td, th')).filter((c) => c.closest('table') === outer)) {
      const d = doc.createElement('div');
      while (cell.firstChild) d.appendChild(cell.firstChild);
      box.appendChild(d);
    }
    outer.replaceWith(box);
  }
  if (keepTables) return;
  for (const table of Array.from(root.querySelectorAll('table'))) {
    const box = doc.createElement('div');
    const cap = table.querySelector('caption');
    if (cap && normText(cap.textContent)) {
      const p = doc.createElement('p');
      const b = doc.createElement('strong');
      b.textContent = normText(cap.textContent);
      p.appendChild(b);
      box.appendChild(p);
    }
    for (const tr of Array.from(table.querySelectorAll('tr'))) {
      const cells = Array.from(tr.querySelectorAll('td, th')).map((c) => normText(c.textContent)).filter(Boolean);
      if (!cells.length) continue;
      const p = doc.createElement('p');
      p.textContent = cells.join(' | ');
      box.appendChild(p);
    }
    table.replaceWith(box);
  }
}
