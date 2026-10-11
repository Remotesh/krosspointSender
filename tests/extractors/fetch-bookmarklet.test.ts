import { afterEach, describe, expect, it, vi } from 'vitest';
import { FetchError, fetchErrorHelp, fetchPage, parseBookmarkletPayload, readPayloadFromHash } from '../../src/extractors';

function toBase64Utf8(s: string): string {
  const bytes = new TextEncoder().encode(s);
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return btoa(bin);
}

describe('fetchPage', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it('maps a TypeError to cors or network', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('Failed to fetch')));
    const err = await fetchPage('https://news.example.com/story').catch((e) => e);
    expect(err).toBeInstanceOf(FetchError);
    expect(['cors', 'network']).toContain(err.code);
    // The test page origin differs from the target, so the code is cors.
    expect(err.code).toBe('cors');
    expect(fetchErrorHelp(err).length).toBeGreaterThan(2);
  });

  it('sends no credentials and asks for HTML', async () => {
    const f = vi.fn().mockResolvedValue(new Response('<p>hi</p>', { status: 200 }));
    vi.stubGlobal('fetch', f);
    const res = await fetchPage('https://news.example.com/story');
    expect(res.html).toBe('<p>hi</p>');
    expect(res.finalUrl).toBe('https://news.example.com/story');
    const init = f.mock.calls[0][1] as RequestInit;
    expect(init.credentials).toBe('omit');
    expect((init.headers as Record<string, string>).Accept).toMatch(/text\/html/);
  });

  it('maps non-2xx to http with status', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('no', { status: 404 })));
    const err = await fetchPage('https://news.example.com/missing').catch((e) => e);
    expect(err).toBeInstanceOf(FetchError);
    expect(err.code).toBe('http');
    expect(err.status).toBe(404);
    expect(fetchErrorHelp(err)[0]).toMatch(/404/);
  });

  it('maps a private host failure to private-network', async () => {
    vi.stubGlobal('fetch', vi.fn().mockRejectedValue(new TypeError('Failed to fetch')));
    vi.stubGlobal('location', { protocol: 'http:', origin: 'http://app.example.org', hostname: 'app.example.org' });
    const err = await fetchPage('http://192.168.1.20/page').catch((e) => e);
    expect(err.code).toBe('private-network');
  });

  it('rejects http from an https page as mixed-content', async () => {
    const f = vi.fn();
    vi.stubGlobal('fetch', f);
    vi.stubGlobal('location', { protocol: 'https:', origin: 'https://k-engine.org', hostname: 'k-engine.org' });
    const err = await fetchPage('http://plain.example.com/a').catch((e) => e);
    expect(err.code).toBe('mixed-content');
    expect(f).not.toHaveBeenCalled();
  });

  it('times out after 20 seconds', async () => {
    vi.useFakeTimers();
    vi.stubGlobal(
      'fetch',
      vi.fn((_url: string, init: RequestInit) =>
        new Promise((_res, rej) => init.signal!.addEventListener('abort', () => rej(new DOMException('aborted', 'AbortError')))),
      ),
    );
    const p = fetchPage('https://slow.example.com/').catch((e) => e);
    await vi.advanceTimersByTimeAsync(20_001);
    const err = await p;
    expect(err).toBeInstanceOf(FetchError);
    expect(err.code).toBe('timeout');
  });
});

describe('parseBookmarkletPayload', () => {
  const payload = { v: 1, url: 'https://ex.com/a', title: 'Ünïcode — title', html: '<p>Grüße</p>', capturedAt: '2026-10-10T10:00:00.000Z' };

  it('accepts JSON', () => {
    expect(parseBookmarkletPayload(JSON.stringify(payload))).toEqual(payload);
  });

  it('accepts base64 JSON with UTF-8', () => {
    expect(parseBookmarkletPayload(toBase64Utf8(JSON.stringify(payload)))).toEqual(payload);
  });

  it('accepts base64url without padding', () => {
    const b64url = toBase64Utf8(JSON.stringify(payload)).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    expect(parseBookmarkletPayload(b64url)).toEqual(payload);
  });

  it('throws on bad input', () => {
    expect(() => parseBookmarkletPayload('')).toThrow();
    expect(() => parseBookmarkletPayload('not base64 !!!')).toThrow();
    expect(() => parseBookmarkletPayload('{"v":1,')).toThrow();
    expect(() => parseBookmarkletPayload('[1,2]')).toThrow();
    expect(() => parseBookmarkletPayload(JSON.stringify({ ...payload, html: '' }))).toThrow();
    expect(() => parseBookmarkletPayload(JSON.stringify({ ...payload, v: 2 }))).toThrow();
    expect(() => parseBookmarkletPayload(JSON.stringify({ ...payload, url: 'javascript:alert(1)' }))).toThrow();
  });

  it('fills capturedAt when missing', () => {
    const p = parseBookmarkletPayload(JSON.stringify({ v: 1, url: 'https://ex.com', html: '<p>x</p>' }));
    expect(Number.isNaN(Date.parse(p.capturedAt))).toBe(false);
  });
});

describe('readPayloadFromHash', () => {
  it('reads #payload= with plain and percent-encoded base64', () => {
    const json = JSON.stringify({ v: 1, url: 'https://ex.com/a', html: '<p>a+b/c</p>', capturedAt: '2026-10-10T10:00:00.000Z' });
    const b64 = toBase64Utf8(json);
    expect(readPayloadFromHash('#payload=' + b64)?.html).toBe('<p>a+b/c</p>');
    expect(readPayloadFromHash('#x=1&payload=' + encodeURIComponent(b64))?.url).toBe('https://ex.com/a');
  });

  it('returns null without a valid payload', () => {
    expect(readPayloadFromHash('')).toBeNull();
    expect(readPayloadFromHash('#section-2')).toBeNull();
    expect(readPayloadFromHash('#payload=%%%bad')).toBeNull();
  });
});

describe('fetchWikipedia', () => {
  it('parses Wikipedia URLs', async () => {
    const { parseWikipediaUrl } = await import('../../src/extractors');
    expect(parseWikipediaUrl('https://en.wikipedia.org/wiki/E_Ink')).toEqual({ lang: 'en', title: 'E Ink', host: 'en.wikipedia.org' });
    expect(parseWikipediaUrl('https://de.m.wikipedia.org/wiki/Brot#History')).toEqual({ lang: 'de', title: 'Brot', host: 'de.wikipedia.org' });
    expect(parseWikipediaUrl('https://example.com/wiki/X')).toBeNull();
  });
  it('calls the MediaWiki API with origin=* and wraps the parser output', async () => {
    const { fetchWikipedia } = await import('../../src/extractors');
    const calls: string[] = [];
    vi.stubGlobal('fetch', vi.fn(async (u: string) => {
      calls.push(u);
      return new Response(JSON.stringify({ parse: { title: 'E Ink', text: '<div class="mw-parser-output"><p>Hello</p></div>' } }), { status: 200 });
    }));
    const r = await fetchWikipedia('https://en.wikipedia.org/wiki/E_Ink');
    expect(calls[0]).toContain('en.wikipedia.org/w/api.php');
    expect(calls[0]).toContain('origin=*');
    expect(calls[0]).toContain('page=E+Ink');
    expect(r.html).toContain('id="firstHeading">E Ink<');
    expect(r.html).toContain('mw-parser-output');
    expect(r.finalUrl).toBe('https://en.wikipedia.org/wiki/E_Ink');
    vi.unstubAllGlobals();
  });
  it('maps an API error to FetchError http', async () => {
    const { fetchWikipedia, FetchError } = await import('../../src/extractors');
    vi.stubGlobal('fetch', vi.fn(async () => new Response(JSON.stringify({ error: { code: 'missingtitle', info: 'The page you specified doesn\'t exist.' } }), { status: 200 })));
    await expect(fetchWikipedia('https://en.wikipedia.org/wiki/Nope')).rejects.toBeInstanceOf(FetchError);
    vi.unstubAllGlobals();
  });
});
