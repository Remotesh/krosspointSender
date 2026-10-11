/**
 * HTML sanitizer. It uses DOMPurify with a strict allowlist.
 * The output is linear, safe HTML for EPUB chapters and previews.
 */
import createDOMPurify from 'dompurify';
import { fixImages, handleTables, linearize, normalizeFigures, parseFragment, removeJunk, unwrap, wrapInlineRuns, normText } from './dom';

export const ALLOWED_TAGS = [
  'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'br', 'hr', 'ul', 'ol', 'li', 'blockquote', 'pre', 'code',
  'b', 'strong', 'i', 'em', 'u', 's', 'del', 'sub', 'sup', 'span', 'a', 'img',
  'figure', 'figcaption', 'table', 'thead', 'tbody', 'tr', 'th', 'td', 'caption', 'dl', 'dt', 'dd',
];

export const ALLOWED_ATTR = ['href', 'src', 'alt', 'id', 'colspan', 'rowspan'];

export interface SanitizeOptions {
  /** Keep <a href> links (default true). If false, links become plain text. */
  keepLinks?: boolean;
  /** Keep images (default true). */
  keepImages?: boolean;
  /** Resolve relative href and src against this URL. */
  baseUrl?: string;
  /** Keep tables (default true). If false, each row becomes a paragraph. */
  keepTables?: boolean;
}

type Purifier = ReturnType<typeof createDOMPurify>;
let purifier: Purifier | null = null;
let currentBase: string | undefined;

function resolveUrl(value: string, base: string | undefined): string {
  if (!base || value.startsWith('#')) return value;
  try {
    return new URL(value, base).toString();
  } catch {
    return value;
  }
}

/** Make one DOMPurify instance with our hook. The hook reads `currentBase`. */
function getPurifier(): Purifier {
  if (purifier) return purifier;
  const p = createDOMPurify(window as unknown as Window & typeof globalThis);
  p.addHook('afterSanitizeAttributes', (node) => {
    const el = node as Element;
    if (!el.getAttribute) return;
    const href = el.getAttribute('href');
    if (href != null) {
      const h = href.trim();
      if (!h || /^(javascript|data|vbscript|file|blob):/i.test(h.replace(/[\s\u0000-\u001f]/g, ''))) el.removeAttribute('href');
      else el.setAttribute('href', resolveUrl(h, currentBase));
    }
    const src = el.getAttribute('src');
    if (src != null) {
      const s = src.trim();
      const isImg = el.tagName === 'IMG';
      if (/^data:/i.test(s)) {
        if (!(isImg && /^data:image\//i.test(s))) el.removeAttribute('src');
      } else if (!s || /^(javascript|vbscript|file|blob):/i.test(s.replace(/[\s\u0000-\u001f]/g, ''))) {
        el.removeAttribute('src');
      } else {
        el.setAttribute('src', resolveUrl(s, currentBase));
      }
    }
  });
  purifier = p;
  return p;
}

/**
 * Sanitize an HTML fragment.
 * Removes page chrome, linearizes the layout, applies the tag and attribute allowlist,
 * resolves relative URLs, and drops dangerous links.
 */
export function sanitizeHtml(html: string, opts: SanitizeOptions = {}): string {
  const keepLinks = opts.keepLinks ?? true;
  const keepImages = opts.keepImages ?? true;

  // 1. Structural clean-up before the allowlist.
  const doc = parseFragment(html ?? '');
  const body = doc.body;
  removeJunk(body, { patterns: false });
  fixImages(body);
  normalizeFigures(body);
  handleTables(body, opts.keepTables ?? true);
  linearize(body);

  // 2. The allowlist.
  const p = getPurifier();
  currentBase = opts.baseUrl;
  let clean: string;
  try {
    clean = p.sanitize(body.innerHTML, {
      ALLOWED_TAGS,
      ALLOWED_ATTR,
      ALLOW_DATA_ATTR: false,
      ALLOW_ARIA_ATTR: false,
      KEEP_CONTENT: true,
      WHOLE_DOCUMENT: false,
      RETURN_TRUSTED_TYPE: false,
    }) as string;
  } finally {
    currentBase = undefined;
  }

  // 3. Clean-up after the allowlist.
  const out = parseFragment(clean);
  const ob = out.body;
  for (const span of Array.from(ob.querySelectorAll('span'))) {
    if (!span.id) unwrap(span);
  }
  for (const a of Array.from(ob.querySelectorAll('a'))) {
    if (!keepLinks || !a.getAttribute('href')) {
      if (a.id && !keepLinks) {
        const anchor = out.createElement('span');
        anchor.id = a.id;
        a.before(anchor);
      }
      unwrap(a);
    }
  }
  for (const img of Array.from(ob.querySelectorAll('img'))) {
    if (!keepImages || !img.getAttribute('src')) {
      img.remove();
    } else if (!img.hasAttribute('alt')) {
      img.setAttribute('alt', '');
    }
  }
  // Remove empty elements, deepest first.
  const emptyCheck = 'p, figure, figcaption, h1, h2, h3, h4, h5, h6, li, blockquote, b, strong, i, em, u, s, del, sub, sup, a, code, pre, caption, dt, dd, ul, ol, dl';
  for (const el of Array.from(ob.querySelectorAll(emptyCheck)).reverse()) {
    if (el.tagName === 'A' && el.id) continue;
    if (el.querySelector('img, hr, table') || normText(el.textContent)) continue;
    el.remove();
  }
  for (const fig of Array.from(ob.querySelectorAll('figure'))) {
    if (!fig.querySelector('img')) unwrap(fig);
  }
  // Remove <br> at the start and the end of a block, and <br> between blocks.
  const isEdge = (br: Element, dir: 'previousSibling' | 'nextSibling') => {
    let n = br[dir];
    while (n && n.nodeType === 3 && !/\S/.test(n.textContent ?? '')) n = n[dir];
    return !n || (n.nodeType === 1 && /^(P|H[1-6]|UL|OL|DL|TABLE|FIGURE|BLOCKQUOTE|PRE|HR)$/.test((n as Element).tagName));
  };
  for (const br of Array.from(ob.querySelectorAll('br'))) {
    if (isEdge(br, 'previousSibling') || isEdge(br, 'nextSibling')) br.remove();
  }
  wrapInlineRuns(ob);
  return ob.innerHTML.trim();
}
