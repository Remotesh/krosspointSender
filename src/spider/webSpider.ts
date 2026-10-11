/**
 * Web spider: candidate pages from the links of one cleaned article.
 */
import type { Article, PackCandidate, PackLimits } from '../types';
import { isNonPageUrl, siteHost, webKey } from './linkFilter';

/**
 * List the links of a cleaned article as candidates.
 * Rules: absolute http(s) only, no fragments, no mailto/javascript, no files
 * that are not pages, no link to the page itself, each key once.
 * With `sameDomainOnly`, only links to the host of `baseUrl` ("www." ignored).
 * All candidates have depth 1 and `selected=false`. Max `maxArticles * 5`.
 */
export function scanWebLinks(article: Article, baseUrl: string, limits: PackLimits): PackCandidate[] {
  const base = baseUrl || article.sourceUrl || '';
  const selfKey = webKey(base);
  const host = siteHost(base);
  const cap = Math.max(1, Math.floor(limits.maxArticles) * 5);
  const doc = new DOMParser().parseFromString(`<!DOCTYPE html><html><body>${article.bodyHtml ?? ''}</body></html>`, 'text/html');
  const seen = new Set<string>();
  const out: PackCandidate[] = [];
  for (const a of Array.from(doc.body.querySelectorAll('a[href]'))) {
    if (out.length >= cap) break;
    const href = (a.getAttribute('href') ?? '').trim();
    if (!href || href.startsWith('#')) continue;
    const key = base ? webKey(href, base) : webKey(href);
    if (!key || key === selfKey || isNonPageUrl(key)) continue;
    // "www." and no "www." are the same page for the duplicate check.
    const loose = key.replace(/^(https?:\/\/)www\./i, '$1');
    if (seen.has(loose) || (selfKey && loose === selfKey.replace(/^(https?:\/\/)www\./i, '$1'))) continue;
    if (limits.sameDomainOnly && siteHost(key) !== host) continue;
    seen.add(loose);
    const text = (a.textContent ?? '').replace(/\s+/g, ' ').trim();
    const cand: PackCandidate = { key, title: text || key, url: key, depth: 1, linkCount: 1, selected: false };
    if (selfKey) cand.parentKey = selfKey;
    out.push(cand);
  }
  return out;
}
