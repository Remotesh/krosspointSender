/**
 * Wikipedia spider: it finds the articles that a root article links to.
 * It uses the MediaWiki API (origin=*), so the browser can read the answers.
 */
import type { PackCandidate, PackLimits, PackProgress } from '../types';
import { fetchWithRetry, parseWikipediaUrl } from '../extractors/fetch';
import { Pacer, abortError, isAbortError, throwIfAborted } from './crawlQueue';
import {
  NON_BODY_SELECTOR,
  canonicalTitle,
  classifyWikiHref,
  hasNonArticleNamespace,
  rememberAlias,
  wikipediaArticleUrl,
} from './linkFilter';

/** Figure and caption containers. Their links come after the body-text links. */
const CAPTION_SELECTOR = 'figure, figcaption, .thumb, .thumbinner, .thumbcaption, .gallery, .gallerybox, .gallerytext, .tsingle, .multiimageinner';

/** Titles per pageprops query (the API limit for normal users). */
const PAGEPROPS_BATCH = 50;

/**
 * Read a Wikipedia URL or a plain title.
 * Accepts desktop and mobile URLs, URLs without a scheme, and titles with "_".
 * Throws an Error when the input is empty, not a Wikipedia URL, or not an article.
 */
export function normalizeWikipediaTitle(input: string, lang = 'en'): { lang: string; title: string; url: string } {
  const s = (input ?? '').trim();
  if (!s) throw new Error('Enter a Wikipedia URL or an article title.');
  let outLang = (lang || 'en').toLowerCase();
  let title: string;
  const looksLikeUrl = /^https?:\/\//i.test(s) || /^\/\//.test(s) || /(^|\.)wikipedia\.org(\/|$)/i.test(s);
  if (looksLikeUrl) {
    const abs = /^https?:\/\//i.test(s) ? s : 'https://' + s.replace(/^\/\//, '');
    const p = parseWikipediaUrl(abs);
    if (!p) throw new Error('This is not a Wikipedia article URL.');
    outLang = p.lang;
    title = canonicalTitle(p.title);
  } else {
    title = canonicalTitle(s.split('#')[0]);
  }
  if (!title) throw new Error('The article title is empty.');
  if (hasNonArticleNamespace(title)) throw new Error(`"${title}" is not an article page.`);
  return { lang: outLang, title, url: wikipediaArticleUrl(outLang, title) };
}

interface ApiError {
  error?: { code?: string; info?: string };
}

/** Called when the API rate-limits a request, with the wait in seconds. */
type OnWait = (seconds: number) => void;

/**
 * Call the MediaWiki API of one language.
 * HTTP 429/503: wait (Retry-After, else 2, 4, 8 s) and try again, max 3 retries.
 * Throws an AbortError when the caller aborts.
 */
async function wikiApi<T>(lang: string, params: Record<string, string>, signal?: AbortSignal, onWait?: OnWait): Promise<T> {
  throwIfAborted(signal);
  const url = new URL(`https://${lang}.wikipedia.org/w/api.php`);
  url.search = new URLSearchParams({ ...params, format: 'json', formatversion: '2', origin: '*' }).toString();
  let res: Response;
  try {
    res = await fetchWithRetry(url.toString(), { method: 'GET', credentials: 'omit' }, { signal, onRetry: onWait ? (w) => onWait(w) : undefined });
  } catch (err) {
    if (signal?.aborted || isAbortError(err)) throw abortError();
    throw new Error(`The request to Wikipedia failed: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (!res.ok) throw new Error(`Wikipedia answered with HTTP ${res.status}.`);
  let data: T & ApiError;
  try {
    data = (await res.json()) as T & ApiError;
  } catch (err) {
    if (signal?.aborted) throw abortError();
    throw new Error(`Wikipedia sent a response that is not JSON: ${err instanceof Error ? err.message : String(err)}`);
  }
  if (data.error) throw new Error(`Wikipedia API error: ${data.error.info || data.error.code || 'unknown'}`);
  return data;
}

interface ParseLinksResponse {
  parse?: {
    title?: string;
    text?: string;
    links?: { ns: number; title: string; exists?: boolean }[];
  };
}

/**
 * Body links of a parsed page, each title once. Links in the body text come first,
 * in order of first appearance; links that occur only in figures and captions come after them.
 * Rejects navigation/infobox/reference links, redlinks, other languages,
 * non-article namespaces and links to the page itself.
 * `linksMeta` (from prop=links) is a second check on the namespace number.
 */
export function extractBodyLinks(
  html: string,
  lang: string,
  selfTitle: string,
  linksMeta: { ns: number; title: string; exists?: boolean }[] = [],
): string[] {
  const meta = new Map<string, { ns: number; exists?: boolean }>();
  for (const l of linksMeta) meta.set(canonicalTitle(l.title), { ns: l.ns, exists: l.exists });
  const doc = new DOMParser().parseFromString(`<!DOCTYPE html><html><body>${html}</body></html>`, 'text/html');
  const root = doc.querySelector('.mw-parser-output') ?? doc.body;
  const self = canonicalTitle(selfTitle);
  const seen = new Set<string>();
  const out: string[] = [];
  const captionLinks: string[] = [];
  for (const a of Array.from(root.querySelectorAll('a[href]'))) {
    if (a.classList.contains('new') || a.classList.contains('extiw') || a.classList.contains('external')) continue;
    if (a.closest(NON_BODY_SELECTOR)) continue;
    const c = classifyWikiHref(a.getAttribute('href') ?? '', lang);
    if (c.type !== 'article' || c.lang !== lang) continue;
    if (c.title === self || seen.has(c.title)) continue;
    const m = meta.get(c.title);
    if (m && (m.ns !== 0 || m.exists === false)) continue;
    if (a.closest(CAPTION_SELECTOR)) {
      if (!captionLinks.includes(c.title)) captionLinks.push(c.title);
      continue;
    }
    seen.add(c.title);
    out.push(c.title);
  }
  // Ranking: body-text links first, then links that occur only in figures and captions.
  return [...out, ...captionLinks.filter((t) => !seen.has(t))];
}

/** Fetch one page and return its canonical title and its body links. */
async function scanPage(lang: string, title: string, signal?: AbortSignal, onWait?: OnWait): Promise<{ title: string; links: string[] }> {
  const data = await wikiApi<ParseLinksResponse>(
    lang,
    { action: 'parse', page: title, prop: 'text|links', redirects: '1', disableeditsection: '1' },
    signal,
    onWait,
  );
  if (!data.parse || typeof data.parse.text !== 'string') throw new Error(`Wikipedia has no text for "${title}".`);
  const canon = canonicalTitle(data.parse.title || title);
  return { title: canon, links: extractBodyLinks(data.parse.text, lang, canon, data.parse.links ?? []) };
}

interface PagePropsResponse {
  query?: {
    normalized?: { from: string; to: string }[];
    redirects?: { from: string; to: string }[];
    pages?: { title: string; missing?: boolean; invalid?: boolean; pageprops?: Record<string, string> }[];
  };
}

interface Entry {
  cand: PackCandidate;
  linkers: Set<string>;
}

/**
 * Scan the links of a root article, breadth-first to `limits.depth`.
 * The root is candidate 0 and selected. Candidates are in order of depth,
 * then first appearance in the body text. Depth-1 candidates are selected
 * until `maxArticles` pages (root included) are selected.
 * Bounds: max `maxArticles * 5` candidates, so max that many page scans.
 * `requestDelayMs` separates all API calls.
 * Throws a DOMException "AbortError" when the signal aborts.
 */
export async function scanWikipediaLinks(
  root: { lang: string; title: string },
  limits: PackLimits,
  onProgress: (p: PackProgress) => void,
  signal?: AbortSignal,
): Promise<PackCandidate[]> {
  throwIfAborted(signal);
  const lang = (root.lang || 'en').toLowerCase();
  const cap = Math.max(1, Math.floor(limits.maxArticles) * 5);
  const pacer = new Pacer(limits.requestDelayMs, signal);
  const errors: PackProgress['errors'] = [];
  let last: Omit<PackProgress, 'errors' | 'phase'> = { done: 0, total: 1 };
  const emit = (p: Omit<PackProgress, 'errors' | 'phase'>) => {
    last = p;
    onProgress({ phase: 'scanning', ...p, errors: [...errors] });
  };
  const onWait: OnWait = (s) => onProgress({ phase: 'scanning', ...last, message: `rate limited, waiting ${s}s`, errors: [...errors] });

  let rootTitle = canonicalTitle(root.title);
  const rootFlags: PackCandidate['flags'] = [];

  const entries = new Map<string, Entry>();
  const add = (title: string, depth: number, parentKey: string | undefined, linker: string | undefined): void => {
    const e = entries.get(title);
    if (e) {
      if (linker && linker !== title) e.linkers.add(linker);
      return;
    }
    if (entries.size >= cap) return;
    const cand: PackCandidate = {
      key: title,
      title,
      url: wikipediaArticleUrl(lang, title),
      depth,
      linkCount: 0,
      selected: false,
    };
    if (parentKey) cand.parentKey = parentKey;
    entries.set(title, { cand, linkers: new Set(linker ? [linker] : []) });
  };

  if (limits.depth === 0) {
    add(rootTitle, 0, undefined, undefined);
    const only = entries.get(rootTitle)!.cand;
    only.selected = true;
    emit({ done: 1, total: 1, currentTitle: rootTitle, message: 'Depth 0: only the root page.' });
    return [only];
  }

  // Depth 0 -> 1: the root page.
  emit({ done: 0, total: 1, currentTitle: rootTitle, message: 'Reading the root article.' });
  await pacer.wait();
  const first = await scanPage(lang, rootTitle, signal, onWait);
  if (first.title !== rootTitle) {
    rememberAlias(lang, rootTitle, first.title);
    rootTitle = first.title;
    rootFlags.push('redirect');
  }
  add(rootTitle, 0, undefined, undefined);
  for (const t of first.links) add(t, 1, rootTitle, rootTitle);

  // Depth 1 -> 2: scan depth-1 pages while there is room for candidates.
  const depth1 = [...entries.values()].filter((e) => e.cand.depth === 1).map((e) => e.cand.key);
  const total = limits.depth >= 2 ? 1 + depth1.length : 1;
  let done = 1;
  emit({ done, total, currentTitle: rootTitle, message: `Found ${entries.size - 1} linked articles.` });
  if (limits.depth >= 2) {
    for (const key of depth1) {
      if (entries.size >= cap) break;
      throwIfAborted(signal);
      emit({ done, total, currentTitle: key });
      try {
        await pacer.wait();
        const res = await scanPage(lang, key, signal, onWait);
        if (res.title !== key) rememberAlias(lang, key, res.title);
        for (const t of res.links) if (t !== key) add(t, 2, key, key);
      } catch (err) {
        if (isAbortError(err)) throw err;
        errors.push({ key, title: key, error: err instanceof Error ? err.message : String(err) });
      }
      done++;
    }
  }

  // Disambiguation and redirect check, 50 titles per request.
  const titles = [...entries.keys()];
  const final = new Map<string, string>(); // asked title -> canonical title
  const disambig = new Set<string>();
  const missing = new Set<string>();
  for (let i = 0; i < titles.length; i += PAGEPROPS_BATCH) {
    const batch = titles.slice(i, i + PAGEPROPS_BATCH);
    emit({ done, total, currentTitle: batch[0], message: 'Checking for disambiguation pages.' });
    try {
      await pacer.wait();
      const data = await wikiApi<PagePropsResponse>(
        lang,
        { action: 'query', prop: 'pageprops', ppprop: 'disambiguation', redirects: '1', titles: batch.join('|') },
        signal,
        onWait,
      );
      const norm = new Map((data.query?.normalized ?? []).map((n) => [n.from, n.to]));
      const redir = new Map((data.query?.redirects ?? []).map((r) => [r.from, r.to]));
      for (const t of batch) {
        let f = norm.get(t) ?? t;
        for (let hop = 0; hop < 5 && redir.has(f); hop++) f = redir.get(f)!;
        final.set(t, canonicalTitle(f));
      }
      for (const p of data.query?.pages ?? []) {
        const pt = canonicalTitle(p.title);
        if (p.missing || p.invalid) missing.add(pt);
        else if (p.pageprops && 'disambiguation' in p.pageprops) disambig.add(pt);
      }
    } catch (err) {
      if (isAbortError(err)) throw err;
      errors.push({ key: batch[0], title: 'Disambiguation check', error: err instanceof Error ? err.message : String(err) });
    }
  }

  // Merge redirects into their targets, drop missing pages. Entries are in BFS order,
  // so the first entry for a canonical title has the lowest depth.
  const merged = new Map<string, Entry>();
  const renamed = new Map<string, string>();
  for (const [key, e] of entries) {
    const isRoot = key === rootTitle;
    const f = final.get(key) ?? key;
    if (!isRoot && missing.has(f)) continue;
    if (f !== key) {
      rememberAlias(lang, key, f);
      renamed.set(key, f);
    }
    const prev = merged.get(f);
    if (prev) {
      e.linkers.forEach((l) => prev.linkers.add(l));
      // The page is also linked by its own title: it is not only a redirect.
      if (f === key && prev.cand.flags?.includes('redirect') && prev.cand.depth !== 0) {
        prev.cand.flags = prev.cand.flags.filter((x) => x !== 'redirect');
        if (!prev.cand.flags.length) delete prev.cand.flags;
      }
      continue;
    }
    if (f !== key) {
      e.cand.key = f;
      e.cand.title = f;
      e.cand.url = wikipediaArticleUrl(lang, f);
      e.cand.flags = [...(e.cand.flags ?? []), 'redirect'];
      if (isRoot) rootTitle = f;
    }
    if (disambig.has(f)) e.cand.flags = [...(e.cand.flags ?? []), 'disambiguation'];
    merged.set(f, e);
  }

  const out: PackCandidate[] = [];
  for (const e of merged.values()) {
    const c = e.cand;
    if (c.parentKey) c.parentKey = renamed.get(c.parentKey) ?? c.parentKey;
    const linkers = new Set([...e.linkers].map((l) => renamed.get(l) ?? l));
    linkers.delete(c.key);
    c.linkCount = linkers.size;
    if (c.depth === 0 && rootFlags.length) c.flags = [...new Set([...(c.flags ?? []), ...rootFlags])];
    out.push(c);
  }
  // Order: depth, then first appearance (the insertion order is stable inside one depth).
  out.sort((a, b) => a.depth - b.depth);

  // Selection: the root, then depth-1 pages that are not disambiguation pages.
  let selected = 0;
  for (const c of out) {
    if (c.depth === 0) {
      c.selected = true;
      selected++;
    } else if (c.depth === 1 && selected < limits.maxArticles && !c.flags?.includes('disambiguation')) {
      c.selected = true;
      selected++;
    } else {
      c.selected = false;
    }
  }

  emit({ done: total, total, currentTitle: rootTitle, message: `Found ${out.length - 1} candidates.` });
  return out;
}
