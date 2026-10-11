import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  DEFAULT_PREFIXES,
  HTTPS_SKIP_REASON,
  candidatePrefixes,
  findReader,
  lanBlocked,
  parsePrefix,
  type FindProgress,
  type FindReaderOptions,
} from '../../src/discovery/findReader';
import { mockResponse } from '../transports/mocks';

const HELPER = 'http://127.0.0.1:8134';
const STATUS = (ip: string, extra: Record<string, unknown> = {}) =>
  JSON.stringify({ version: '1.2.0', ip, mode: 'STA', rssi: -50, device: 'X4Pro', ...extra });

type Reply = { status?: number; body?: string } | 'fail';

/**
 * Stubs fetch. The handler maps a URL to a reply ('fail' = network error).
 * Each reply waits delayMs (honours the abort signal). Tracks requests in flight.
 */
function stubNet(handler: (url: string) => Reply, delayMs = 0) {
  const urls: string[] = [];
  const stats = { inFlight: 0, maxInFlight: 0 };
  const fn = vi.fn(async (url: string, init: RequestInit = {}) => {
    urls.push(String(url));
    stats.inFlight++;
    stats.maxInFlight = Math.max(stats.maxInFlight, stats.inFlight);
    try {
      await new Promise<void>((resolve, reject) => {
        const sig = init.signal;
        if (sig?.aborted) return reject(Object.assign(new Error('aborted'), { name: 'AbortError' }));
        const t = setTimeout(resolve, delayMs);
        sig?.addEventListener('abort', () => {
          clearTimeout(t);
          reject(Object.assign(new Error('aborted'), { name: 'AbortError' }));
        });
      });
      const r = handler(String(url));
      if (r === 'fail') throw new TypeError('Failed to fetch');
      return mockResponse(r);
    } finally {
      stats.inFlight--;
    }
  });
  vi.stubGlobal('fetch', fn);
  return { urls, stats };
}

/** Options for a plain http page with no helper unless asked. */
const base = (o: FindReaderOptions = {}): FindReaderOptions => ({
  helperUrl: false,
  protocol: 'http:',
  origin: 'http://localhost:5173',
  probeTimeoutMs: 200,
  mdnsTimeoutMs: 200,
  helperTimeoutMs: 200,
  helperDiscoverTimeoutMs: 200,
  ...o,
});

const sweepUrls = (urls: string[]) => urls.filter((u) => /^http:\/\/\d+\.\d+\.\d+\.\d+\/api\/status$/.test(u));

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('prefix helpers', () => {
  it('parses user subnets', () => {
    expect(parsePrefix('192.168.1')).toBe('192.168.1');
    expect(parsePrefix(' 192.168.1. ')).toBe('192.168.1');
    expect(parsePrefix('192.168.1.x')).toBe('192.168.1');
    expect(parsePrefix('192.168.1.0/24')).toBe('192.168.1');
    expect(parsePrefix('192.168.1.25')).toBe('192.168.1');
    expect(parsePrefix('010.0.0')).toBe('10.0.0');
    expect(parsePrefix('192.168')).toBeUndefined();
    expect(parsePrefix('192.168.300')).toBeUndefined();
    expect(parsePrefix('home')).toBeUndefined();
    expect(parsePrefix('')).toBeUndefined();
  });

  it('orders candidates: user, profile /24s, defaults; no repeats; private only', () => {
    expect(candidatePrefixes()).toEqual(DEFAULT_PREFIXES);
    expect(candidatePrefixes('172.20.5', ['192.168.1.50', 'crosspoint.local', '10.1.2.3', '8.8.8.8', '192.168.1.60'])).toEqual([
      '172.20.5',
      '192.168.1',
      '10.1.2',
      '192.168.0',
      '192.168.4',
      '10.0.0',
      '192.168.2',
      '192.168.50',
    ]);
    // A public user prefix is not swept.
    expect(candidatePrefixes('8.8.8')[0]).toBe('192.168.0');
  });

  it('knows when the LAN is blocked', () => {
    expect(lanBlocked('https:', 'https://k-engine.org')).toBe(true);
    expect(lanBlocked('https:', 'https://localhost:5173')).toBe(false);
    expect(lanBlocked('https:', 'https://127.0.0.1')).toBe(false);
    expect(lanBlocked('http:', 'http://localhost:5173')).toBe(false);
    expect(lanBlocked('file:', 'null')).toBe(false);
  });
});

describe('findReader', () => {
  it('uses the helper /discover results', async () => {
    const { urls } = stubNet((u) => {
      if (u === `${HELPER}/status`) return { body: '{"ok":true,"version":"1.1.0"}' };
      if (u === `${HELPER}/discover`)
        return { body: JSON.stringify({ ok: true, devices: [{ ip: '192.168.1.77', hostname: 'CrossPoint-AB12', wsPort: 81, raw: 'x' }, { ip: '' }] }) };
      return 'fail';
    });
    const stages: string[] = [];
    const res = await findReader(base({ helperUrl: HELPER }), (p) => stages.push(p.message));
    expect(res.readers).toEqual([{ host: '192.168.1.77', name: 'CrossPoint-AB12', source: 'helper' }]);
    expect(stages[0]).toBe('Asking the desktop helper…');
    // Found by the helper: mDNS still runs, the sweep does not.
    expect(urls).toContain('http://crosspoint.local/api/status');
    expect(sweepUrls(urls)).toEqual([]);
    expect(res.cancelled).toBe(false);
  });

  it('is quiet when the helper does not run or has no /discover', async () => {
    stubNet((u) => (u === `${HELPER}/status` ? { body: '{"ok":true}' } : u === `${HELPER}/discover` ? { status: 404, body: '{}' } : 'fail'));
    const res = await findReader(base({ helperUrl: HELPER, subnet: '192.168.9', thorough: false }));
    expect(res.readers).toEqual([]);
    expect(res.notes.some((n) => n.includes('/discover'))).toBe(true);
  });

  it('finds the reader by crosspoint.local', async () => {
    const { urls } = stubNet((u) => (u === 'http://crosspoint.local/api/status' ? { body: STATUS('192.168.0.42') } : 'fail'));
    const msgs: string[] = [];
    const res = await findReader(base(), (p) => msgs.push(p.message));
    expect(msgs).toContain('Trying crosspoint.local…');
    expect(res.readers).toEqual([{ host: 'crosspoint.local', ip: '192.168.0.42', name: 'X4Pro', version: '1.2.0', source: 'mdns' }]);
    expect(sweepUrls(urls)).toEqual([]);
  });

  it('ignores a crosspoint.local answer that is not CrossPoint', async () => {
    stubNet((u) => (u === 'http://crosspoint.local/api/status' ? { body: '{"hello":1}' } : 'fail'));
    const res = await findReader(base({ subnet: '192.168.7', profileHosts: [] }), () => {});
    expect(res.readers).toEqual([]);
  });

  it('sweeps, counts only CrossPoint answers, and stops after the prefix with a hit', async () => {
    const { urls } = stubNet((u) => {
      if (u === 'http://192.168.1.23/api/status') return { body: STATUS('192.168.1.23') };
      if (u === 'http://192.168.1.1/api/status') return { body: '<html>router</html>' };
      if (u === 'http://192.168.1.2/api/status') return { body: '{"version":"x"}' }; // no "mode"
      return 'fail';
    });
    const progress: FindProgress[] = [];
    const res = await findReader(base(), (p) => progress.push(p));
    expect(res.readers).toEqual([{ host: '192.168.1.23', name: 'X4Pro', version: '1.2.0', source: 'sweep' }]);
    expect(res.swept).toEqual(['192.168.0', '192.168.1']);
    const s = sweepUrls(urls);
    expect(s).toHaveLength(254 * 2);
    expect(s.some((u) => u.startsWith('http://192.168.4.'))).toBe(false);
    expect(progress.some((p) => p.message === 'Sweeping 192.168.0.x (0%)')).toBe(true);
    expect(progress.some((p) => /^Sweeping 192\.168\.1\.x \(\d+%\)$/.test(p.message))).toBe(true);
    expect(progress.some((p) => p.message === 'Sweeping 192.168.1.x (100%)')).toBe(true);
    expect(progress.at(-1)?.stage).toBe('done');
  });

  it('sweeps profile prefixes first, after the user prefix', async () => {
    const { urls } = stubNet(() => 'fail');
    const ctrl = new AbortController();
    const seen: string[] = [];
    await findReader(base({ signal: ctrl.signal, subnet: '10.9.9', profileHosts: ['192.168.50.4', '172.16.0.9'] }), (p) => {
      if (p.prefix && !seen.includes(p.prefix)) seen.push(p.prefix);
      if (seen.length === 3) ctrl.abort();
    });
    expect(seen).toEqual(['10.9.9', '192.168.50', '172.16.0']);
    expect(sweepUrls(urls)[0]).toBe('http://10.9.9.1/api/status');
  });

  it('skips mDNS and the sweep on an https page, with the reason, and still tries the helper', async () => {
    const { urls } = stubNet(() => 'fail');
    const res = await findReader(base({ protocol: 'https:', origin: 'https://k-engine.org', helperUrl: HELPER }));
    expect(urls).toEqual([`${HELPER}/status`]);
    expect(res.readers).toEqual([]);
    expect(res.notes).toContain(HTTPS_SKIP_REASON);
    expect(HTTPS_SKIP_REASON).toContain('Save an offline copy');
    expect(res.cancelled).toBe(false);
  });

  it('still returns helper readers on an https page', async () => {
    stubNet((u) =>
      u === `${HELPER}/status` ? { body: '{"ok":true}' } : u === `${HELPER}/discover` ? { body: '{"ok":true,"devices":[{"ip":"10.0.0.8"}]}' } : 'fail',
    );
    const res = await findReader(base({ protocol: 'https:', origin: 'https://k-engine.org', helperUrl: HELPER }));
    expect(res.readers.map((r) => r.host)).toEqual(['10.0.0.8']);
    expect(res.notes).toContain(HTTPS_SKIP_REASON);
  });

  it('stops mid-sweep on cancel and keeps what it found', async () => {
    const { urls } = stubNet((u) => (u === 'http://192.168.0.3/api/status' ? { body: STATUS('192.168.0.3') } : 'fail'), 5);
    const ctrl = new AbortController();
    const res = await findReader(base({ signal: ctrl.signal }), (p) => {
      if (p.stage === 'sweep' && (p.fraction ?? 0) > 0.3) ctrl.abort();
    });
    expect(res.cancelled).toBe(true);
    expect(res.readers.map((r) => r.host)).toEqual(['192.168.0.3']);
    const n = sweepUrls(urls).length;
    expect(n).toBeLessThan(254);
    // No new requests after the cancel.
    await new Promise((r) => setTimeout(r, 30));
    expect(sweepUrls(urls).length).toBe(n);
  });

  it('returns at once when cancelled before the start', async () => {
    const { urls } = stubNet(() => 'fail');
    const ctrl = new AbortController();
    ctrl.abort();
    const res = await findReader(base({ signal: ctrl.signal, helperUrl: HELPER }));
    expect(res.cancelled).toBe(true);
    expect(urls).toEqual([]);
  });

  it('dedupes by host and by the mDNS ip', async () => {
    stubNet((u) => {
      if (u === `${HELPER}/status`) return { body: '{"ok":true}' };
      if (u === `${HELPER}/discover`)
        return { body: JSON.stringify({ devices: [{ ip: '192.168.2.5' }, { ip: '192.168.2.5' }, { ip: '192.168.2.9' }] }) };
      if (u === 'http://crosspoint.local/api/status') return { body: STATUS('192.168.2.9') };
      if (u === 'http://192.168.2.5/api/status' || u === 'http://192.168.2.9/api/status' || u === 'http://192.168.2.11/api/status')
        return { body: STATUS(u.slice(7, -11)) };
      return 'fail';
    });
    const res = await findReader(base({ helperUrl: HELPER, subnet: '192.168.2' }));
    expect(res.readers.map((r) => `${r.host}:${r.source}`)).toEqual(['192.168.2.5:helper', '192.168.2.9:helper', '192.168.2.11:sweep']);
    // The user prefix is swept even though the helper found readers; nothing else is.
    expect(res.swept).toEqual(['192.168.2']);
  });

  it('never has more than 32 requests in flight', async () => {
    const { stats } = stubNet(() => 'fail', 2);
    const ctrl = new AbortController();
    await findReader(base({ signal: ctrl.signal, concurrency: 100 }), (p) => {
      if (p.prefix === '192.168.1') ctrl.abort();
    });
    expect(stats.maxInFlight).toBe(32);
  });

  it('respects a lower concurrency', async () => {
    const { stats } = stubNet(() => 'fail', 1);
    const ctrl = new AbortController();
    await findReader(base({ signal: ctrl.signal, subnet: '192.168.3', concurrency: 4 }), (p) => {
      if (p.prefix && p.prefix !== '192.168.3') ctrl.abort();
    });
    expect(stats.maxInFlight).toBe(4);
  });

  it('times out a slow probe', async () => {
    const { urls } = stubNet((u) => (u.includes('crosspoint.local') ? { body: STATUS('1.1.1.1') } : 'fail'), 100);
    const ctrl = new AbortController();
    const res = await findReader(base({ signal: ctrl.signal, mdnsTimeoutMs: 20 }), (p) => {
      if (p.stage === 'sweep') ctrl.abort();
    });
    expect(urls[0]).toBe('http://crosspoint.local/api/status');
    expect(res.readers).toEqual([]);
    expect(res.notes).toContain('crosspoint.local did not answer.');
  });
});
