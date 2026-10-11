/**
 * Link rewriting between the pages of a pack.
 */
import type { Pack, PackPage, PackSourceKind } from '../types';
import { packSlug } from './slug';
import { canonicalTitle, classifyWikiHref, resolveAlias, webKey, wikipediaArticleUrl } from './linkFilter';

/** File names that the EPUB builder uses for its own pages. */
const RESERVED = ['index', 'nav', 'toc', 'external-links', 'missing', 'attribution', 'cover', 'title'];

/** Give each page a unique file name. Existing names stay. */
function assignFileNames(pages: PackPage[]): PackPage[] {
  const used = new Set<string>(RESERVED.map((r) => `${r}.xhtml`));
  for (const p of pages) if (p.fileName) used.add(p.fileName);
  return pages.map((p) => {
    if (p.fileName) return { ...p };
    const base = packSlug(p.article?.title || p.candidate.title);
    let name = `${base}.xhtml`;
    for (let i = 2; used.has(name); i++) name = `${base}_${i}.xhtml`;
    used.add(name);
    return { ...p, fileName: name };
  });
}

function langOf(url: string | undefined): string {
  const m = /^https?:\/\/([a-z-]+)\.(?:m\.)?wikipedia\.org/i.exec(url ?? '');
  return m && m[1].toLowerCase() !== 'm' ? m[1].toLowerCase() : 'en';
}

function unwrap(el: Element): void {
  const parent = el.parentNode;
  if (!parent) return;
  while (el.firstChild) parent.insertBefore(el.firstChild, el);
  parent.removeChild(el);
}

function linkText(a: Element): string {
  const clone = a.cloneNode(true) as Element;
  clone.querySelectorAll('span.ext').forEach((s) => s.remove());
  return (clone.textContent ?? '').replace(/\s+/g, ' ').trim();
}

/** Mark a link as external: class "ext" and a "↗" marker inside the anchor. */
function markExternal(a: Element): void {
  a.classList.add('ext');
  if (a.querySelector('span.ext')) return;
  const span = a.ownerDocument.createElement('span');
  span.setAttribute('class', 'ext');
  span.textContent = '↗';
  a.appendChild(span);
}

/**
 * Rewrite the links in the body of each fetched page.
 * - A link to a bundled page becomes href="<fileName>" (the fragment is dropped).
 * - A link to a known page that is not bundled keeps its URL, gets class "ext"
 *   and a "↗" marker, and goes into `external` (one entry per key, with the
 *   first page that holds it).
 * - Citation and reference links keep their URL. A "#..." link stays when its
 *   target id is in the page; else it is unwrapped.
 * - Junk links (edit, redlink, non-article namespaces, javascript) are unwrapped.
 * Each page gets a unique `fileName` (packSlug + ".xhtml") when it has none.
 * The input pages are not changed.
 */
export function rewriteLinks(pages: PackPage[], kind: PackSourceKind): { pages: PackPage[]; external: Pack['externalLinks'] } {
  const named = assignFileNames(pages);
  const lang = langOf(named[0]?.candidate.url);
  const fileNames = new Set(named.map((p) => p.fileName!));

  // Target key -> file name, for bundled pages only.
  const bundled = new Map<string, string>();
  for (const p of named) {
    if (!p.article) continue;
    const keys = kind === 'wikipedia' ? [canonicalTitle(p.candidate.key), canonicalTitle(p.article.title)] : [
      webKey(p.candidate.key), webKey(p.candidate.url), p.article.sourceUrl ? webKey(p.article.sourceUrl) : null,
    ];
    for (const k of keys) if (k && !bundled.has(k)) bundled.set(k, p.fileName!);
  }

  const external: Pack['externalLinks'] = [];
  const externalKeys = new Set<string>();
  const addExternal = (key: string, title: string, url: string, fromKey: string) => {
    if (externalKeys.has(key)) return;
    externalKeys.add(key);
    external.push({ key, title, url, fromKey });
  };

  const out = named.map((page) => {
    if (!page.article) return page;
    const doc = new DOMParser().parseFromString(`<!DOCTYPE html><html><body>${page.article.bodyHtml}</body></html>`, 'text/html');
    const ids = new Set(Array.from(doc.body.querySelectorAll('[id]')).map((e) => e.getAttribute('id')));
    const own = page.fileName!;
    const fromKey = page.candidate.key;
    const base = page.article.sourceUrl || page.candidate.url;

    for (const a of Array.from(doc.body.querySelectorAll('a'))) {
      const href = (a.getAttribute('href') ?? '').trim();
      if (!href) {
        unwrap(a);
        continue;
      }
      if (fileNames.has(href.split('#')[0])) continue; // Already rewritten.
      if (href.startsWith('#')) {
        if (!ids.has(href.slice(1)) && !ids.has(decodeURIComponentSafe(href.slice(1)))) unwrap(a);
        continue;
      }
      if (/^(javascript|vbscript|data):/i.test(href)) {
        unwrap(a);
        continue;
      }

      if (kind === 'wikipedia') {
        const c = classifyWikiHref(href, lang);
        if (c.type === 'junk') unwrap(a);
        else if (c.type === 'fragment') {
          if (!ids.has(c.fragment)) unwrap(a);
        } else if (c.type === 'article') {
          if (c.lang !== lang) continue; // Another language: keep the URL.
          const key = resolveAlias(lang, c.title);
          const target = bundled.get(key) ?? bundled.get(c.title);
          if (target) {
            a.setAttribute('href', target === own && c.fragment && ids.has(c.fragment) ? `#${c.fragment}` : target);
          } else {
            const url = wikipediaArticleUrl(lang, key);
            a.setAttribute('href', url);
            markExternal(a);
            addExternal(key, key, url, fromKey);
          }
        }
        // 'other': a citation or reference URL on another site. Keep it.
        continue;
      }

      // Web packs.
      const key = webKey(href, base);
      if (!key) continue; // mailto: and similar stay.
      const target = bundled.get(key);
      if (target) {
        a.setAttribute('href', target);
      } else {
        markExternal(a);
        addExternal(key, linkText(a) || key, key, fromKey);
      }
    }
    return { ...page, article: { ...page.article, bodyHtml: doc.body.innerHTML } };
  });

  return { pages: out, external };
}

function decodeURIComponentSafe(s: string): string {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
}
