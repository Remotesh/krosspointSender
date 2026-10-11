/**
 * Pack creation and page fetching.
 */
import type { Article, ExtractOptions, Pack, PackLimits, PackPage, PackProgress, PackSourceKind } from '../types';
import { fetchPage, fetchWikipedia } from '../extractors/fetch';
import { extractWikipedia } from '../extractors/wikipedia';
import { extractArticle } from '../extractors/article';
import { newId } from '../extractors/finish';
import { Pacer, abortError, isAbortError, throwIfAborted } from './crawlQueue';
import { canonicalTitle, rememberAlias } from './linkFilter';

export const DEFAULT_PACK_LIMITS: PackLimits = {
  depth: 1,
  maxArticles: 20,
  maxImagesPerArticle: 2,
  maxTotalEpubSizeMB: 25,
  requestDelayMs: 500,
  includeReferences: false,
  includeInfobox: false,
  sameDomainOnly: true,
};

/** Make an empty pack. The caller adds pages (one per selected candidate). */
export function newPack(kind: PackSourceKind, rootKey: string, rootUrl: string, title: string, limits: PackLimits): Pack {
  const now = new Date().toISOString();
  return {
    id: newId(),
    kind,
    title,
    rootKey,
    rootUrl,
    limits: { ...limits },
    pages: [],
    externalLinks: [],
    createdAt: now,
    updatedAt: now,
  };
}

/** Keep the first `max` images. Remove the other images from the body too. */
function limitImages(article: Article, max: number): Article {
  const limit = Math.max(0, Math.floor(max));
  if (article.images.length <= limit) return article;
  const keep = new Set(article.images.slice(0, limit).map((i) => i.src));
  const doc = new DOMParser().parseFromString(`<!DOCTYPE html><html><body>${article.bodyHtml}</body></html>`, 'text/html');
  for (const img of Array.from(doc.body.querySelectorAll('img'))) {
    if (!keep.has(img.getAttribute('src') ?? '')) img.remove();
  }
  for (const fig of Array.from(doc.body.querySelectorAll('figure'))) {
    if (!fig.querySelector('img')) fig.remove();
  }
  return { ...article, bodyHtml: doc.body.innerHTML, images: article.images.slice(0, limit) };
}

function errorText(err: unknown): string {
  return err instanceof Error ? err.message : String(err);
}

async function fetchOne(
  pack: Pack,
  page: PackPage,
  opts: ExtractOptions,
  signal?: AbortSignal,
  onWait?: (seconds: number) => void,
): Promise<Article> {
  const url = page.candidate.url;
  let article: Article;
  if (pack.kind === 'wikipedia') {
    const { html, finalUrl } = await fetchWikipedia(url, signal, onWait);
    article = extractWikipedia(html, finalUrl, opts);
    const lang = /^https?:\/\/([a-z-]+)\./i.exec(finalUrl)?.[1]?.toLowerCase() ?? 'en';
    if (canonicalTitle(article.title) !== page.candidate.key) rememberAlias(lang, article.title, page.candidate.key);
  } else {
    const { html, finalUrl } = await fetchPage(url, signal);
    article = await extractArticle({ kind: 'url', content: html, sourceUrl: finalUrl }, opts);
  }
  return limitImages(article, pack.limits.maxImagesPerArticle);
}

/**
 * Fetch the selected pages of a pack, one at a time, in order (root first).
 * - Pages with `candidate.selected === false` get state 'skipped'.
 * - Pages that are 'done' with an article are not fetched again.
 * - A failed page gets state 'failed' and an error; the next page continues.
 * - `requestDelayMs` separates the fetches. Wikipedia HTTP 429/503 answers are
 *   tried again after a wait (message "rate limited, waiting Ns").
 * The input pack is not changed. The function throws only on abort
 * (DOMException "AbortError").
 */
export async function fetchPackPages(
  pack: Pack,
  extractOpts: ExtractOptions,
  onProgress: (p: PackProgress) => void,
  signal?: AbortSignal,
): Promise<Pack> {
  throwIfAborted(signal);
  const pages: PackPage[] = pack.pages.map((p) => ({ ...p, candidate: { ...p.candidate } }));
  const rootIdx = pages.findIndex((p) => p.candidate.key === pack.rootKey);
  if (rootIdx > 0) pages.unshift(...pages.splice(rootIdx, 1));

  const opts: ExtractOptions = {
    ...extractOpts,
    keepInfobox: pack.limits.includeInfobox,
    keepReferences: pack.limits.includeReferences,
    keepLinks: extractOpts.keepLinks ?? true,
  };
  const work: PackPage[] = [];
  for (const p of pages) {
    if (p.candidate.selected === false) p.state = 'skipped';
    else if (p.state === 'done' && p.article) continue;
    else {
      p.state = 'queued';
      work.push(p);
    }
  }

  const errors: PackProgress['errors'] = [];
  const total = work.length;
  let done = 0;
  const emit = (phase: PackProgress['phase'], currentTitle?: string, message?: string) =>
    onProgress({ phase, done, total, currentTitle, message, errors: [...errors] });
  const pacer = new Pacer(pack.limits.requestDelayMs, signal);

  for (const page of work) {
    const title = page.candidate.title;
    page.state = 'fetching';
    delete page.error;
    emit('fetching', title);
    try {
      await pacer.wait();
      page.article = await fetchOne(pack, page, opts, signal, (s) => emit('fetching', title, `rate limited, waiting ${s}s`));
      page.state = 'done';
    } catch (err) {
      if (signal?.aborted || isAbortError(err)) {
        page.state = 'queued';
        emit('cancelled', title, 'Cancelled.');
        throw abortError();
      }
      page.state = 'failed';
      page.error = errorText(err);
      errors.push({ key: page.candidate.key, title, error: page.error });
    }
    done++;
    emit('fetching', title);
  }

  const failedAll = total > 0 && errors.length === total;
  emit(failedAll ? 'failed' : 'complete', undefined, failedAll ? 'No page could be fetched.' : `Fetched ${total - errors.length} of ${total} pages.`);
  return { ...pack, pages, updatedAt: new Date().toISOString() };
}
