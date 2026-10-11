/**
 * Test helpers: a mocked MediaWiki API.
 */
import { vi } from 'vitest';
import type { Article, PackCandidate, PackLimits, PackPage } from '../../src/types';

export const LIMITS: PackLimits = {
  depth: 1,
  maxArticles: 20,
  maxImagesPerArticle: 2,
  maxTotalEpubSizeMB: 25,
  requestDelayMs: 0,
  includeReferences: false,
  includeInfobox: false,
  sameDomainOnly: true,
};

export interface Call {
  url: URL;
  at: number;
}

/** Stub fetch. The handler gets the query parameters and returns a JSON body or a Response. */
export function mockApi(handler: (q: URLSearchParams, url: URL) => unknown): Call[] {
  const calls: Call[] = [];
  vi.stubGlobal(
    'fetch',
    vi.fn(async (input: string, init?: RequestInit) => {
      if (init?.signal?.aborted) throw new DOMException('aborted', 'AbortError');
      const url = new URL(String(input));
      calls.push({ url, at: Date.now() });
      const body = handler(url.searchParams, url);
      if (body instanceof Response) return body;
      return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } });
    }),
  );
  return calls;
}

/** A parse response whose body links to the given titles, in order. */
export function parseBody(title: string, links: string[], extraHtml = ''): unknown {
  const anchors = links.map((l) => `<a href="/wiki/${encodeURIComponent(l.replace(/ /g, '_'))}" title="${l}">${l}</a>`).join(', ');
  return {
    parse: {
      title,
      text: `<div class="mw-parser-output"><p>${title} links to ${anchors}.</p>${extraHtml}</div>`,
      links: links.map((l) => ({ ns: 0, title: l, exists: true })),
    },
  };
}

/** A pageprops response with every title present and no flags. */
export function plainPageProps(q: URLSearchParams): unknown {
  return { query: { pages: (q.get('titles') ?? '').split('|').map((t, i) => ({ pageid: i + 1, ns: 0, title: t })) } };
}

export function cand(key: string, url: string, extra: Partial<PackCandidate> = {}): PackCandidate {
  return { key, title: key, url, depth: 1, linkCount: 1, selected: true, ...extra };
}

export function article(title: string, bodyHtml: string, sourceUrl: string): Article {
  return {
    id: 'id-' + title,
    title,
    capturedAt: '2026-10-10T00:00:00.000Z',
    sourceUrl,
    tags: [],
    bodyHtml,
    toc: [],
    images: [],
    wordCount: 10,
    inputKind: 'wikipedia',
  };
}

export function wikiPage(title: string, bodyHtml?: string, extra: Partial<PackPage> = {}): PackPage {
  const url = `https://en.wikipedia.org/wiki/${title.replace(/ /g, '_')}`;
  return {
    candidate: cand(title, url),
    state: bodyHtml === undefined ? 'failed' : 'done',
    article: bodyHtml === undefined ? undefined : article(title, bodyHtml, url),
    ...extra,
  };
}
