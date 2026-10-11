/**
 * TOC builder and word count.
 */
import type { TocEntry } from '../types';
import { normText, parseFragment } from './dom';

/**
 * Give every heading an id ("sec-1", "sec-2", ...) and list the headings.
 * Internal links to the old ids follow the new ids.
 */
export function buildToc(bodyHtml: string): { bodyHtml: string; toc: TocEntry[] } {
  const doc = parseFragment(bodyHtml ?? '');
  const toc: TocEntry[] = [];
  const renamed = new Map<string, string>();
  let n = 0;
  for (const h of Array.from(doc.body.querySelectorAll('h1, h2, h3, h4, h5, h6'))) {
    n += 1;
    const id = `sec-${n}`;
    const old = h.getAttribute('id');
    if (old && old !== id) renamed.set(old, id);
    // An anchor inside the heading can carry the target id.
    for (const inner of Array.from(h.querySelectorAll('[id]'))) {
      renamed.set(inner.getAttribute('id')!, id);
      inner.removeAttribute('id');
    }
    h.setAttribute('id', id);
    const text = normText(h.textContent);
    if (text) toc.push({ id, level: Number(h.tagName[1]), text });
  }
  // Other elements must not reuse a sec-N id.
  for (const el of Array.from(doc.body.querySelectorAll('[id^="sec-"]'))) {
    if (!/^H[1-6]$/.test(el.tagName)) el.removeAttribute('id');
  }
  if (renamed.size) {
    for (const a of Array.from(doc.body.querySelectorAll('a[href^="#"]'))) {
      const target = decodeURIComponent(a.getAttribute('href')!.slice(1));
      const next = renamed.get(target);
      if (next) a.setAttribute('href', `#${next}`);
    }
  }
  return { bodyHtml: doc.body.innerHTML, toc };
}

/** Count whitespace-separated tokens of the text content. */
export function countWords(html: string): number {
  const doc = parseFragment(html ?? '');
  // Separate blocks so "a</p><p>b" counts as two words.
  for (const el of Array.from(doc.body.querySelectorAll('p, li, h1, h2, h3, h4, h5, h6, td, th, br, div, blockquote, figcaption, dt, dd, pre, caption'))) {
    el.appendChild(doc.createTextNode(' '));
  }
  const text = doc.body.textContent ?? '';
  const words = text.split(/\s+/).filter(Boolean);
  return words.length;
}
