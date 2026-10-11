import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { extractBodyLinks, normalizeWikipediaTitle, scanWikipediaLinks } from '../../src/spider';
import { clearAliases } from '../../src/spider/linkFilter';
import type { PackProgress } from '../../src/types';
import { LIMITS, mockApi, parseBody, plainPageProps } from './helpers';
import ROOT_PARSE from '../fixtures/spider/parse-steam-engine.json';
import ROOT_PROPS from '../fixtures/spider/pageprops-steam-engine.json';


const EXPECTED_BODY = ['Heat engine', 'Work (physics)', 'Steam', 'Mercury (disambiguation)', 'Steam engines', 'Thomas Newcomen', 'James Watt'];

beforeEach(() => clearAliases());
afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('normalizeWikipediaTitle', () => {
  it('reads a desktop URL', () => {
    expect(normalizeWikipediaTitle('https://en.wikipedia.org/wiki/Steam_engine')).toEqual({
      lang: 'en',
      title: 'Steam engine',
      url: 'https://en.wikipedia.org/wiki/Steam_engine',
    });
  });

  it('reads a plain title with underscores and a lower-case first letter', () => {
    expect(normalizeWikipediaTitle('steam_engine')).toMatchObject({ lang: 'en', title: 'Steam engine' });
    expect(normalizeWikipediaTitle('  Heat   engine ')).toMatchObject({ title: 'Heat engine' });
    expect(normalizeWikipediaTitle('Dampfmaschine', 'de')).toEqual({
      lang: 'de',
      title: 'Dampfmaschine',
      url: 'https://de.wikipedia.org/wiki/Dampfmaschine',
    });
  });

  it('reads mobile hosts, URLs without a scheme, fragments and encoded titles', () => {
    expect(normalizeWikipediaTitle('https://de.m.wikipedia.org/wiki/Dampfmaschine#Geschichte')).toMatchObject({
      lang: 'de',
      title: 'Dampfmaschine',
    });
    expect(normalizeWikipediaTitle('en.m.wikipedia.org/wiki/Heat_engine')).toMatchObject({ lang: 'en', title: 'Heat engine' });
    const fr = normalizeWikipediaTitle('https://fr.wikipedia.org/wiki/Machine_%C3%A0_vapeur');
    expect(fr.title).toBe('Machine à vapeur');
    expect(fr.url).toBe('https://fr.wikipedia.org/wiki/Machine_%C3%A0_vapeur');
  });

  it('rejects empty input, other sites and non-article namespaces', () => {
    expect(() => normalizeWikipediaTitle('')).toThrow();
    expect(() => normalizeWikipediaTitle('https://example.com/wiki/X')).toThrow();
    expect(() => normalizeWikipediaTitle('Help:Contents')).toThrow();
  });
});

describe('extractBodyLinks', () => {
  it('keeps body article links in order and drops namespaces and junk', () => {
    const links = extractBodyLinks(ROOT_PARSE.parse.text, 'en', 'Steam engine', ROOT_PARSE.parse.links);
    expect(links).toEqual(EXPECTED_BODY);
  });

  it('uses the ns number from prop=links as a second check', () => {
    const html = '<div class="mw-parser-output"><p><a href="/wiki/Foo:Bar">x</a> <a href="/wiki/Ok">ok</a></p></div>';
    expect(extractBodyLinks(html, 'en', 'Root', [{ ns: 4, title: 'Foo:Bar' }, { ns: 0, title: 'Ok' }])).toEqual(['Ok']);
  });
});

describe('scanWikipediaLinks', () => {
  it('scans depth 1, merges redirects, flags disambiguation pages and selects up to maxArticles', async () => {
    const calls = mockApi((q) => (q.get('action') === 'parse' ? ROOT_PARSE : ROOT_PROPS));
    const progress: PackProgress[] = [];
    const out = await scanWikipediaLinks({ lang: 'en', title: 'steam_engine' }, { ...LIMITS, maxArticles: 3 }, (p) => progress.push(p));

    expect(out.map((c) => c.key)).toEqual(['Steam engine', 'Heat engine', 'Work (physics)', 'Steam', 'Mercury (disambiguation)', 'Thomas Newcomen', 'James Watt']);
    expect(out[0]).toMatchObject({ depth: 0, selected: true, url: 'https://en.wikipedia.org/wiki/Steam_engine' });
    expect(out.filter((c) => c.selected).map((c) => c.key)).toEqual(['Steam engine', 'Heat engine', 'Work (physics)']);
    const merc = out.find((c) => c.key === 'Mercury (disambiguation)')!;
    expect(merc.flags).toContain('disambiguation');
    expect(merc.selected).toBe(false);
    expect(out.slice(1).every((c) => c.depth === 1 && c.parentKey === 'Steam engine' && c.linkCount === 1)).toBe(true);

    // API rules.
    expect(calls).toHaveLength(2);
    for (const c of calls) {
      expect(c.url.searchParams.get('origin')).toBe('*');
      expect(c.url.searchParams.get('format')).toBe('json');
      expect(c.url.searchParams.get('formatversion')).toBe('2');
      expect(c.url.hostname).toBe('en.wikipedia.org');
    }
    expect(calls[0].url.searchParams.get('prop')).toBe('text|links');
    expect(calls[1].url.searchParams.get('ppprop')).toBe('disambiguation');
    expect(calls[1].url.searchParams.get('titles')!.split('|')).toContain('Steam engines');
    expect(progress.every((p) => p.phase === 'scanning')).toBe(true);
  });

  it('skips disambiguation pages when it selects, so a later page takes the place', async () => {
    mockApi((q) => (q.get('action') === 'parse' ? ROOT_PARSE : ROOT_PROPS));
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Steam engine' }, { ...LIMITS, maxArticles: 6 }, () => {});
    expect(out.filter((c) => c.selected).map((c) => c.key)).toEqual(['Steam engine', 'Heat engine', 'Work (physics)', 'Steam', 'Thomas Newcomen', 'James Watt']);
  });

  it('batches the disambiguation check at 50 titles', async () => {
    const many = Array.from({ length: 70 }, (_, i) => `Page ${i}`);
    const calls = mockApi((q) => (q.get('action') === 'parse' ? parseBody('Root', many) : plainPageProps(q)));
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, maxArticles: 20 }, () => {});
    expect(out).toHaveLength(71);
    const props = calls.filter((c) => c.url.searchParams.get('action') === 'query');
    expect(props).toHaveLength(2);
    expect(props[0].url.searchParams.get('titles')!.split('|')).toHaveLength(50);
    expect(props[1].url.searchParams.get('titles')!.split('|')).toHaveLength(21);
  });

  it('orders depth 2 after depth 1 and counts linking pages', async () => {
    const pages: Record<string, string[]> = {
      Root: ['A', 'B', 'C'],
      A: ['B', 'C', 'D'],
      B: ['C', 'Root'],
      C: [],
    };
    mockApi((q) => {
      if (q.get('action') === 'parse') return parseBody(q.get('page')!, pages[q.get('page')!] ?? []);
      return plainPageProps(q);
    });
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, depth: 2 }, () => {});
    expect(out.map((c) => [c.key, c.depth, c.linkCount])).toEqual([
      ['Root', 0, 1],
      ['A', 1, 1],
      ['B', 1, 2],
      ['C', 1, 3],
      ['D', 2, 1],
    ]);
    expect(out.find((c) => c.key === 'D')).toMatchObject({ parentKey: 'A', selected: false });
  });

  it('never collects more than maxArticles * 5 candidates', async () => {
    const calls = mockApi((q) => {
      if (q.get('action') !== 'parse') return plainPageProps(q);
      const page = q.get('page')!;
      return parseBody(page, Array.from({ length: 4 }, (_, i) => `${page} child ${i}`));
    });
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, depth: 2, maxArticles: 2 }, () => {});
    expect(out).toHaveLength(10);
    // Root scan + depth-1 scans until the cap: 1 + 4 + ... stops early.
    const parses = calls.filter((c) => c.url.searchParams.get('action') === 'parse');
    expect(parses.length).toBeLessThanOrEqual(10);
    expect(parses.length).toBe(3); // Root (5 cands), child 0 (9), child 1 (10 = cap).

    const calls2 = mockApi((q) => (q.get('action') === 'parse' ? parseBody('Root', Array.from({ length: 30 }, (_, i) => `L${i}`)) : plainPageProps(q)));
    const out2 = await scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, depth: 2, maxArticles: 1 }, () => {});
    expect(out2).toHaveLength(5);
    expect(calls2.filter((c) => c.url.searchParams.get('action') === 'parse')).toHaveLength(1);
  });

  it('keeps requestDelayMs between all API calls', async () => {
    vi.useFakeTimers();
    const pages: Record<string, string[]> = { Root: ['A', 'B'], A: [], B: [] };
    const calls = mockApi((q) => (q.get('action') === 'parse' ? parseBody(q.get('page')!, pages[q.get('page')!] ?? []) : plainPageProps(q)));
    const promise = scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, depth: 2, requestDelayMs: 1000 }, () => {});
    await vi.advanceTimersByTimeAsync(0);
    expect(calls).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(999);
    expect(calls).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(calls).toHaveLength(2);
    await vi.runAllTimersAsync();
    await promise;
    expect(calls).toHaveLength(4); // Root, A, B, pageprops.
    for (let i = 1; i < calls.length; i++) expect(calls[i].at - calls[i - 1].at).toBeGreaterThanOrEqual(1000);
  });

  it('rejects with an AbortError when the signal aborts during the delay', async () => {
    vi.useFakeTimers();
    const calls = mockApi((q) => (q.get('action') === 'parse' ? parseBody(q.get('page')!, ['A', 'B']) : plainPageProps(q)));
    const ctrl = new AbortController();
    const promise = scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, depth: 2, requestDelayMs: 5000 }, () => {}, ctrl.signal);
    const check = expect(promise).rejects.toMatchObject({ name: 'AbortError' });
    await vi.advanceTimersByTimeAsync(10);
    ctrl.abort();
    await check;
    expect(calls).toHaveLength(1);
    expect((await promise.catch((e) => e)) instanceof DOMException).toBe(true);
  });

  it('rejects at once when the signal is already aborted', async () => {
    const calls = mockApi(() => ({}));
    const ctrl = new AbortController();
    ctrl.abort();
    await expect(scanWikipediaLinks({ lang: 'en', title: 'Root' }, LIMITS, () => {}, ctrl.signal)).rejects.toMatchObject({ name: 'AbortError' });
    expect(calls).toHaveLength(0);
  });

  it('records a failed depth-1 scan and continues', async () => {
    mockApi((q) => {
      if (q.get('action') !== 'parse') return plainPageProps(q);
      const page = q.get('page')!;
      if (page === 'A') return { error: { code: 'missingtitle', info: 'The page does not exist.' } };
      return parseBody(page, page === 'Root' ? ['A', 'B'] : ['Z']);
    });
    const progress: PackProgress[] = [];
    const out = await scanWikipediaLinks({ lang: 'en', title: 'Root' }, { ...LIMITS, depth: 2 }, (p) => progress.push(p));
    expect(out.map((c) => c.key)).toEqual(['Root', 'A', 'B', 'Z']);
    expect(progress.at(-1)!.errors).toEqual([{ key: 'A', title: 'A', error: expect.stringContaining('does not exist') }]);
  });

  it('returns only the root at depth 0 without a request', async () => {
    const calls = mockApi(() => ({}));
    const out = await scanWikipediaLinks({ lang: 'en', title: 'root page' }, { ...LIMITS, depth: 0 }, () => {});
    expect(out).toEqual([expect.objectContaining({ key: 'Root page', depth: 0, selected: true })]);
    expect(calls).toHaveLength(0);
  });
});
