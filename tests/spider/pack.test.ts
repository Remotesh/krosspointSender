import { afterEach, describe, expect, it, vi } from 'vitest';
import { DEFAULT_PACK_LIMITS, fetchPackPages, newPack, scanWebLinks } from '../../src/spider';
import type { PackPage, PackProgress } from '../../src/types';
import { LIMITS, article, cand, mockApi } from './helpers';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

const W = (t: string) => `https://en.wikipedia.org/wiki/${t.replace(/ /g, '_')}`;

function queued(key: string, extra: Partial<PackPage['candidate']> = {}): PackPage {
  return { candidate: cand(key, W(key), extra), state: 'queued' };
}

function wikiText(title: string): unknown {
  return {
    parse: {
      title,
      text:
        `<div class="mw-parser-output"><p>${title} is a page with enough words to be a real paragraph of text for the test.</p>` +
        `<p><img src="//upload.wikimedia.org/${title}-1.jpg" alt="one"></p><p><img src="//upload.wikimedia.org/${title}-2.jpg" alt="two"></p>` +
        `<p><img src="//upload.wikimedia.org/${title}-3.jpg" alt="three"></p></div>`,
    },
  };
}

describe('newPack and defaults', () => {
  it('makes an empty pack with the limits', () => {
    const p = newPack('wikipedia', 'Steam engine', W('Steam engine'), 'Steam engine', DEFAULT_PACK_LIMITS);
    expect(p).toMatchObject({ kind: 'wikipedia', rootKey: 'Steam engine', pages: [], externalLinks: [] });
    expect(p.id).toMatch(/^[0-9a-f-]{36}$/);
    expect(p.limits).toEqual(DEFAULT_PACK_LIMITS);
    expect(p.limits).not.toBe(DEFAULT_PACK_LIMITS);
    expect(DEFAULT_PACK_LIMITS).toMatchObject({ depth: 1, maxArticles: 20, maxImagesPerArticle: 2, maxTotalEpubSizeMB: 25, requestDelayMs: 500 });
  });
});

describe('fetchPackPages', () => {
  it('fetches in order with the root first, records a failure and continues', async () => {
    const calls = mockApi((q) => {
      const page = q.get('page')!;
      if (page === 'Broken') return { error: { code: 'missingtitle', info: "The page you specified doesn't exist." } };
      return wikiText(page);
    });
    const pack = newPack('wikipedia', 'Root', W('Root'), 'Root', { ...LIMITS, maxImagesPerArticle: 1 });
    pack.pages = [queued('Alpha'), queued('Root', { depth: 0 }), queued('Broken'), queued('Unwanted', { selected: false }), queued('Beta')];
    const snapshot = JSON.stringify(pack);
    const progress: PackProgress[] = [];

    const out = await fetchPackPages(pack, {}, (p) => progress.push(p));

    expect(JSON.stringify(pack)).toBe(snapshot); // Input not changed.
    expect(out.pages.map((p) => [p.candidate.key, p.state])).toEqual([
      ['Root', 'done'],
      ['Alpha', 'done'],
      ['Broken', 'failed'],
      ['Unwanted', 'skipped'],
      ['Beta', 'done'],
    ]);
    expect(calls.map((c) => c.url.searchParams.get('page'))).toEqual(['Root', 'Alpha', 'Broken', 'Beta']);
    const broken = out.pages[2];
    expect(broken.error).toContain("doesn't exist");
    expect(broken.article).toBeUndefined();

    const last = progress.at(-1)!;
    expect(last.phase).toBe('complete');
    expect(last.errors).toEqual([{ key: 'Broken', title: 'Broken', error: broken.error }]);
    expect(last).toMatchObject({ done: 4, total: 4 });
    expect(progress.some((p) => p.phase === 'fetching' && p.currentTitle === 'Alpha' && p.done === 1)).toBe(true);

    // Images truncated to maxImagesPerArticle, in the list and in the body.
    const root = out.pages[0].article!;
    expect(root.title).toBe('Root');
    expect(root.images).toHaveLength(1);
    expect((root.bodyHtml.match(/<img/g) ?? []).length).toBe(1);
    // References and infobox follow the pack limits.
    expect(root.inputKind).toBe('wikipedia');
  });

  it('does not fetch a page that is already done', async () => {
    const calls = mockApi((q) => wikiText(q.get('page')!));
    const pack = newPack('wikipedia', 'Root', W('Root'), 'Root', LIMITS);
    pack.pages = [{ ...queued('Root'), state: 'done', article: article('Root', '<p>x</p>', W('Root')) }, queued('Alpha')];
    const out = await fetchPackPages(pack, {}, () => {});
    expect(calls).toHaveLength(1);
    expect(out.pages.map((p) => p.state)).toEqual(['done', 'done']);
  });

  it('reports failed when every page fails, and never throws for it', async () => {
    mockApi(() => new Response('no', { status: 500 }));
    const pack = newPack('wikipedia', 'Root', W('Root'), 'Root', LIMITS);
    pack.pages = [queued('Root'), queued('Alpha')];
    const progress: PackProgress[] = [];
    const out = await fetchPackPages(pack, {}, (p) => progress.push(p));
    expect(out.pages.every((p) => p.state === 'failed' && p.error)).toBe(true);
    expect(progress.at(-1)!.phase).toBe('failed');
    expect(progress.at(-1)!.errors).toHaveLength(2);
  });

  it('keeps requestDelayMs between fetches and aborts with an AbortError', async () => {
    vi.useFakeTimers();
    const calls = mockApi((q) => wikiText(q.get('page')!));
    const pack = newPack('wikipedia', 'Root', W('Root'), 'Root', { ...LIMITS, requestDelayMs: 2000 });
    pack.pages = [queued('Root'), queued('Alpha'), queued('Beta')];
    const ctrl = new AbortController();
    const progress: PackProgress[] = [];
    const promise = fetchPackPages(pack, {}, (p) => progress.push(p), ctrl.signal);
    const check = expect(promise).rejects.toMatchObject({ name: 'AbortError' });
    await vi.advanceTimersByTimeAsync(1999);
    expect(calls).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(1);
    expect(calls).toHaveLength(2);
    ctrl.abort();
    await check;
    expect(calls).toHaveLength(2);
    expect(progress.at(-1)!.phase).toBe('cancelled');
  });
});

describe('scanWebLinks', () => {
  const body =
    '<p><a href="https://blog.example.com/post-2">Post two</a> <a href="/post-3#part">Post three</a> ' +
    '<a href="https://www.blog.example.com/post-2/">dup</a> <a href="https://other.org/page">Other</a> ' +
    '<a href="mailto:me@example.com">mail</a> <a href="#top">top</a> <a href="https://blog.example.com/file.pdf">pdf</a> ' +
    '<a href="https://blog.example.com/post-1#comments">self</a> <a href="https://blog.example.com/post-4?utm_source=x">Post four</a></p>';
  const a = { ...article('Post one', body, 'https://blog.example.com/post-1'), inputKind: 'url' as const };

  it('keeps same-domain page links once, without fragments or tracking', () => {
    const out = scanWebLinks(a, 'https://blog.example.com/post-1', LIMITS);
    expect(out.map((c) => c.key)).toEqual([
      'https://blog.example.com/post-2',
      'https://blog.example.com/post-3',
      'https://blog.example.com/post-4',
    ]);
    expect(out[0]).toMatchObject({ title: 'Post two', depth: 1, selected: false, parentKey: 'https://blog.example.com/post-1' });
  });

  it('keeps other domains when sameDomainOnly is false', () => {
    const out = scanWebLinks(a, 'https://blog.example.com/post-1', { ...LIMITS, sameDomainOnly: false });
    expect(out.map((c) => c.key)).toContain('https://other.org/page');
    expect(out.map((c) => c.key)).not.toContain('https://blog.example.com/file.pdf');
  });
});
