/**
 * Follow-up tests: caption ranking, redirect flags, rate-limit retry, external marker coverage.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { extractBodyLinks, fetchPackPages, newPack, rewriteLinks, scanWikipediaLinks } from '../../src/spider';
import { clearAliases } from '../../src/spider/linkFilter';
import { fetchWikipedia, parseRetryAfter } from '../../src/extractors/fetch';
import type { PackPage, PackProgress } from '../../src/types';
import { LIMITS, cand, mockApi, parseBody, plainPageProps, wikiPage } from './helpers';
import CAPTION_PARSE from '../fixtures/spider/parse-caption-redirect.json';

const W = 'https://en.wikipedia.org/wiki/';

beforeEach(() => clearAliases());
afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

function withRedirect(q: URLSearchParams, from: string, to: string): unknown {
  const base = plainPageProps(q) as { query: { pages: unknown[] } };
  return { query: { ...base.query, redirects: [{ from, to }] } };
}

function body(html: string): Document {
  return new DOMParser().parseFromString(`<body>${html}</body>`, 'text/html');
}

describe('caption links and redirect flags', () => {
  it('ranks body-text links before figure caption links', () => {
    const links = extractBodyLinks(CAPTION_PARSE.parse.text, 'en', 'Steam engine', CAPTION_PARSE.parse.links);
    expect(links).toEqual(['Heat engine', 'Steam', 'Stott Park Bobbin Mill', 'Ljubljana, Slovenia']);
  });

  it('keeps a title in body position when it is in a caption first and in the text later', () => {
    const html =
      '<div class="mw-parser-output"><figure><figcaption><a href="/wiki/Steam">steam</a></figcaption></figure>' +
      '<p><a href="/wiki/Heat_engine">heat</a> <a href="/wiki/Steam">steam</a></p></div>';
    expect(extractBodyLinks(html, 'en', 'Root')).toEqual(['Heat engine', 'Steam']);
  });

  it('flags a caption link whose own title is a redirect (live Steam engine case)', async () => {
    mockApi((q) => (q.get('action') === 'parse' ? CAPTION_PARSE : withRedirect(q, 'Ljubljana, Slovenia', 'Ljubljana')));
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Steam engine' }, LIMITS, () => {});
    expect(out.map((c) => c.key)).toEqual(['Steam engine', 'Heat engine', 'Steam', 'Stott Park Bobbin Mill', 'Ljubljana']);
    expect(out[4]).toMatchObject({ flags: ['redirect'], url: 'https://en.wikipedia.org/wiki/Ljubljana' });
    expect(out.slice(0, 4).every((c) => !c.flags)).toBe(true);
  });

  it('does not flag redirect when the page is also linked by its own title', async () => {
    mockApi((q) =>
      q.get('action') === 'parse'
        ? parseBody('Root', ['Ljubljana, Slovenia', 'Steam', 'Ljubljana'])
        : withRedirect(q, 'Ljubljana, Slovenia', 'Ljubljana'),
    );
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Root' }, LIMITS, () => {});
    expect(out.map((c) => c.key)).toEqual(['Root', 'Ljubljana', 'Steam']);
    expect(out[1].flags).toBeUndefined();
  });
});

describe('rate limits (HTTP 429/503)', () => {
  it('parses Retry-After as seconds or a date', () => {
    expect(parseRetryAfter('5')).toBe(5);
    expect(parseRetryAfter(null)).toBeNull();
    expect(parseRetryAfter('soon')).toBeNull();
    expect(parseRetryAfter(new Date(10_000).toUTCString(), 4_000)).toBe(6);
  });

  it('scan: waits Retry-After seconds, reports it, and tries again', async () => {
    vi.useFakeTimers();
    let n = 0;
    const calls = mockApi((q) => {
      if (q.get('action') === 'parse' && n++ === 0) return new Response('slow down', { status: 429, headers: { 'Retry-After': '3' } });
      return q.get('action') === 'parse' ? parseBody('Root', ['A']) : plainPageProps(q);
    });
    const progress: PackProgress[] = [];
    const promise = scanWikipediaLinks({ lang: 'en', title: 'Root' }, LIMITS, (p) => progress.push(p));
    await vi.advanceTimersByTimeAsync(2999);
    expect(calls).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(calls.filter((c) => c.url.searchParams.get('action') === 'parse')).toHaveLength(2);
    await vi.runAllTimersAsync();
    const out = await promise;
    expect(out.map((c) => c.key)).toEqual(['Root', 'A']);
    expect(progress.some((p) => p.message === 'rate limited, waiting 3s')).toBe(true);
  });

  it('scan: backs off 2, 4, 8 s without Retry-After and gives up after 3 retries', async () => {
    vi.useFakeTimers();
    const calls = mockApi(() => new Response('busy', { status: 503 }));
    const progress: PackProgress[] = [];
    const promise = scanWikipediaLinks({ lang: 'en', title: 'Root' }, LIMITS, (p) => progress.push(p));
    const check = expect(promise).rejects.toThrow(/HTTP 503/);
    await vi.runAllTimersAsync();
    await check;
    expect(calls).toHaveLength(4);
    expect(calls[1].at - calls[0].at).toBe(2000);
    expect(calls[2].at - calls[1].at).toBe(4000);
    expect(calls[3].at - calls[2].at).toBe(8000);
    expect(progress.filter((p) => p.message?.startsWith('rate limited')).map((p) => p.message)).toEqual([
      'rate limited, waiting 2s',
      'rate limited, waiting 4s',
      'rate limited, waiting 8s',
    ]);
  });

  it('scan: aborts during a rate-limit wait', async () => {
    vi.useFakeTimers();
    const calls = mockApi(() => new Response('slow', { status: 429 }));
    const ctrl = new AbortController();
    const promise = scanWikipediaLinks({ lang: 'en', title: 'Root' }, LIMITS, () => {}, ctrl.signal);
    const check = expect(promise).rejects.toMatchObject({ name: 'AbortError' });
    await vi.advanceTimersByTimeAsync(500);
    ctrl.abort();
    await check;
    expect(calls).toHaveLength(1);
  });

  it('fetchWikipedia: 429 then 200 succeeds and calls onRetry', async () => {
    vi.useFakeTimers();
    let first = true;
    const calls = mockApi(() => {
      if (first) {
        first = false;
        return new Response('slow', { status: 429, headers: { 'Retry-After': '1' } });
      }
      return { parse: { title: 'E Ink', text: '<div class="mw-parser-output"><p>x</p></div>' } };
    });
    const waits: number[] = [];
    const promise = fetchWikipedia('https://en.wikipedia.org/wiki/E_Ink', undefined, (s) => waits.push(s));
    await vi.runAllTimersAsync();
    const r = await promise;
    expect(r.finalUrl).toBe('https://en.wikipedia.org/wiki/E_Ink');
    expect(calls).toHaveLength(2);
    expect(waits).toEqual([1]);
  });

  it('fetchPackPages: retries a 429 and reports the wait on the page', async () => {
    vi.useFakeTimers();
    let first = true;
    const calls = mockApi((q) => {
      if (first) {
        first = false;
        return new Response('slow', { status: 429 });
      }
      const t = q.get('page')!;
      return { parse: { title: t, text: `<div class="mw-parser-output"><p>${t} text for the test paragraph here.</p></div>` } };
    });
    const pack = newPack('wikipedia', 'Root', `${W}Root`, 'Root', LIMITS);
    const page: PackPage = { candidate: cand('Root', `${W}Root`, { depth: 0 }), state: 'queued' };
    pack.pages = [page];
    const progress: PackProgress[] = [];
    const promise = fetchPackPages(pack, {}, (p) => progress.push(p));
    await vi.runAllTimersAsync();
    const out = await promise;
    expect(calls).toHaveLength(2);
    expect(calls[1].at - calls[0].at).toBe(2000);
    expect(out.pages[0].state).toBe('done');
    expect(progress.some((p) => p.message === 'rate limited, waiting 2s' && p.currentTitle === 'Root')).toBe(true);
  });
});

describe('rewriteLinks external marker coverage', () => {
  it('marks every same-wiki article link that is not bundled, and no other-site link', () => {
    const titles = Array.from({ length: 40 }, (_, i) => `Topic_${i}`);
    const html =
      '<p>' +
      titles.map((t) => `<a href="${W}${t}">${t}</a>`).join(' ') +
      ' <a href="https://books.google.com/books?id=abc">book</a> <a href="https://archive.org/details/x">archive</a>' +
      ` <a href="https://en.m.wikipedia.org/wiki/Mobile_topic">m</a> <a href="${W}Heat_engine">bundled</a></p>`;
    const { pages, external } = rewriteLinks([wikiPage('Steam engine', html), wikiPage('Heat engine', '<p>h</p>')], 'wikipedia');
    const doc = body(pages[0].article!.bodyHtml);
    const wiki = Array.from(doc.querySelectorAll('a[href*="wikipedia.org/wiki/"]'));
    expect(wiki).toHaveLength(41);
    expect(wiki.every((a) => a.classList.contains('ext') && a.querySelector('span.ext')?.textContent === '↗')).toBe(true);
    for (const a of Array.from(doc.querySelectorAll('a[href*="books.google"], a[href*="archive.org"]'))) {
      expect(a.classList.contains('ext')).toBe(false);
      expect(a.querySelector('span.ext')).toBeNull();
    }
    expect(doc.querySelector('a[href="heat_engine.xhtml"]')!.classList.contains('ext')).toBe(false);
    expect(external).toHaveLength(41);
    // Idempotent.
    const again = rewriteLinks(pages, 'wikipedia');
    expect(again.pages[0].article!.bodyHtml).toBe(pages[0].article!.bodyHtml);
    expect(again.external).toHaveLength(41);
  });
});
