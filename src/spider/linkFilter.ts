/**
 * Link classification rules for Wikipedia and web packs.
 */

/** Namespace names (lowercase) that are not articles. "X talk" is also rejected. */
const NON_ARTICLE_NS = new Set([
  'help', 'file', 'image', 'media', 'template', 'category', 'talk', 'special', 'portal', 'user',
  'wikipedia', 'wp', 'project', 'draft', 'module', 'mediawiki', 'timedtext', 'book', 'gadget',
  'gadget definition', 'topic', 'education program',
]);

/**
 * HTML containers whose links are not body links
 * (navigation boxes, infoboxes, references, hatnotes, the TOC and similar).
 */
export const NON_BODY_SELECTOR = [
  '.navbox', '.vertical-navbox', '.sidebar', '.infobox', '.reflist', '.mw-references-wrap', 'sup.reference',
  '.reference', '.hatnote', '.catlinks', '#toc', '.toc', '.mw-editsection', '.mw-cite-backlink', '.metadata',
  '.ambox', '.navbox-styles', '.mw-authority-control', '.portal', '.sistersitebox', 'ol.references',
].join(',');

/** Make a title canonical: "_" to space, one space, first letter upper case. */
export function canonicalTitle(raw: string): string {
  const t = (raw ?? '').replace(/_/g, ' ').replace(/\s+/g, ' ').trim();
  if (!t) return '';
  const chars = [...t];
  chars[0] = chars[0].toUpperCase();
  return chars.join('');
}

/** True when the title starts with a non-article namespace prefix. */
export function hasNonArticleNamespace(title: string): boolean {
  const i = title.indexOf(':');
  if (i <= 0) return false;
  const prefix = title.slice(0, i).replace(/_/g, ' ').trim().toLowerCase();
  return NON_ARTICLE_NS.has(prefix) || prefix.endsWith(' talk');
}

/** Article URL for a title. */
export function wikipediaArticleUrl(lang: string, title: string): string {
  const path = encodeURIComponent(title.replace(/ /g, '_')).replace(/%2F/g, '/').replace(/%3A/g, ':');
  return `https://${lang}.wikipedia.org/wiki/${path}`;
}

export function safeDecode(s: string): string {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
}

/** The result of a Wikipedia href classification. */
export type WikiHref =
  | { type: 'article'; lang: string; title: string; fragment: string }
  | { type: 'fragment'; fragment: string }
  | { type: 'junk' }
  | { type: 'other' };

/**
 * Classify a Wikipedia href.
 * `pageLang` resolves relative hrefs ("/wiki/X", "./X").
 * - article: a namespace-0 article link (title canonical, fragment without "#").
 * - fragment: a "#..." link inside the same page.
 * - junk: an edit link, a redlink, a non-article namespace or another wiki path.
 * - other: a link to a site that is not Wikipedia (keep the URL).
 */
export function classifyWikiHref(href: string, pageLang: string): WikiHref {
  const h = (href ?? '').trim();
  if (!h) return { type: 'junk' };
  if (h.startsWith('#')) return { type: 'fragment', fragment: h.slice(1) };
  if (/^(javascript|vbscript|data):/i.test(h)) return { type: 'junk' };
  let u: URL;
  try {
    const abs = h.startsWith('./') ? `/wiki/${h.slice(2)}` : h;
    u = new URL(abs.startsWith('//') ? 'https:' + abs : abs, `https://${pageLang}.wikipedia.org/wiki/`);
  } catch {
    return { type: 'junk' };
  }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return { type: 'other' };
  const m = /^(?:([a-z-]+)\.)?(?:m\.)?wikipedia\.org$/i.exec(u.hostname);
  if (!m) return { type: 'other' };
  const lang = (m[1] && m[1].toLowerCase() !== 'm' ? m[1] : 'en').toLowerCase();
  const fragment = safeDecode(u.hash.replace(/^#/, ''));
  const q = u.searchParams;
  if (q.has('action') || q.has('redlink') || q.has('oldid') || q.has('diff') || q.has('veaction')) return { type: 'junk' };
  let raw = '';
  const wiki = /^\/wiki\/(.+)$/.exec(u.pathname);
  if (wiki) raw = wiki[1];
  else if (u.pathname === '/w/index.php' && q.get('title')) raw = q.get('title') ?? '';
  if (!raw) return { type: 'junk' };
  const title = canonicalTitle(safeDecode(raw));
  if (!title || hasNonArticleNamespace(title)) return { type: 'junk' };
  return { type: 'article', lang, title, fragment };
}

/** File extensions that are not web pages. */
const NON_PAGE_EXT =
  /\.(pdf|jpe?g|png|gif|webp|svg|bmp|ico|zip|gz|tgz|rar|7z|mp3|mp4|m4a|ogg|wav|webm|mov|avi|exe|dmg|apk|docx?|xlsx?|pptx?|epub|mobi|css|js|json|xml|rss|atom)$/i;

const TRACKING = new Set(['fbclid', 'gclid', 'dclid', 'msclkid', 'mc_cid', 'mc_eid', 'ref_src', 'igshid']);

/**
 * Stable key for a web page URL.
 * Absolute, without the fragment and tracking parameters, lower-case host,
 * no trailing slash (except the root path). Returns null for a non-http(s) URL.
 */
export function webKey(href: string, base?: string): string | null {
  const h = (href ?? '').trim();
  if (!h || /^(javascript|vbscript|data|mailto|tel|file|blob|about):/i.test(h)) return null;
  let u: URL;
  try {
    u = base ? new URL(h, base) : new URL(h);
  } catch {
    return null;
  }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return null;
  u.hash = '';
  for (const key of [...u.searchParams.keys()]) {
    const k = key.toLowerCase();
    if (k.startsWith('utm_') || TRACKING.has(k)) u.searchParams.delete(key);
  }
  if (u.pathname.length > 1) u.pathname = u.pathname.replace(/\/+$/, '') || '/';
  let out = u.toString();
  if (!u.search) out = out.replace(/\?$/, '');
  return out;
}

/** True when the URL path looks like a file that is not a web page. */
export function isNonPageUrl(url: string): boolean {
  try {
    return NON_PAGE_EXT.test(new URL(url).pathname);
  } catch {
    return true;
  }
}

/** Host without "www." for same-domain checks. */
export function siteHost(url: string): string {
  try {
    return new URL(url).hostname.toLowerCase().replace(/^www\./, '');
  } catch {
    return '';
  }
}

/**
 * Redirect aliases that the scan and the fetch learned.
 * Key: "<lang>|<title as linked>", value: canonical title.
 * rewriteLinks uses them, so a link to a redirect finds the bundled page.
 */
const aliases = new Map<string, string>();

export function rememberAlias(lang: string, from: string, to: string): void {
  const f = canonicalTitle(from);
  const t = canonicalTitle(to);
  if (f && t && f !== t) aliases.set(`${lang}|${f}`, t);
}

export function resolveAlias(lang: string, title: string): string {
  return aliases.get(`${lang}|${title}`) ?? title;
}

/** Remove all aliases (for tests). */
export function clearAliases(): void {
  aliases.clear();
}
