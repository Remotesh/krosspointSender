import { afterEach, describe, expect, it, vi } from 'vitest';
import { TransferError, type TransferProgress } from '../../src/types';
import {
  getTransport,
  listTransports,
  getHelperTransport,
  explainBlock,
  toTransferError,
  isMixedContent,
  normalizeFolder,
  crc32,
} from '../../src/transports';
import { networkError } from '../../src/transports/common';
import { outputFile, profile, stubFetch, stubXhr, type MockResponseInit } from './mocks';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

function collector() {
  const events: TransferProgress[] = [];
  return { events, cb: (p: TransferProgress) => events.push(p), states: () => events.map((e) => e.state) };
}

describe('registry', () => {
  it('returns one singleton per method', () => {
    expect(getTransport('webdav')).toBe(getTransport('webdav'));
    expect(getTransport('crosspoint-http')).not.toBe(getTransport('webdav'));
    expect(getHelperTransport()).toBe(getHelperTransport());
  });

  it('every adapter implements the interface', () => {
    const all = [...listTransports(), getHelperTransport()];
    expect(listTransports().map((t) => t.method).sort()).toEqual(
      ['ble', 'crosspoint-http', 'crosspoint-ws', 'download', 'opds', 'usb', 'webdav'].sort(),
    );
    for (const t of all) {
      expect(typeof t.method).toBe('string');
      expect(typeof t.isSupported()).toBe('boolean');
      expect(Array.isArray(t.limitations())).toBe(true);
      expect(typeof t.testConnection).toBe('function');
      expect(typeof t.send).toBe('function');
    }
    expect(getTransport('download').isSupported()).toBe(true);
  });

  it('LAN adapters reject with a TransferError on a failing fetch and XHR', async () => {
    for (const m of ['webdav', 'crosspoint-http'] as const) {
      stubFetch(() => new TypeError('Failed to fetch'));
      stubXhr(() => 'network-error');
      const c = collector();
      const err = await getTransport(m).send(profile({ method: m }), outputFile(), c.cb).catch((e) => e);
      expect(err).toBeInstanceOf(TransferError);
      expect(err.code).toBe('network');
      expect(err.message.length).toBeGreaterThan(10);
      expect(c.states().at(-1)).toBe('failed');
    }
  });
});

describe('error mapping', () => {
  it('maps a TypeError on an https page to mixed-content', () => {
    expect(networkError('192.168.1.50', 'https:').code).toBe('mixed-content');
    expect(networkError('192.168.1.50', 'http:').code).toBe('network');
    expect(networkError('localhost', 'https:').code).toBe('network');
    expect(toTransferError(new TypeError('x'), '10.0.0.2', 'https:').code).toBe('mixed-content');
    expect(isMixedContent('127.0.0.1', 'https:')).toBe(false);
  });

  it('normalizes folders', () => {
    expect(normalizeFolder('/Books//Inbox')).toBe('/Books/Inbox/');
    expect(normalizeFolder('/')).toBe('/');
    expect(normalizeFolder('')).toBe('/');
  });

  it('explainBlock names mixed content, Private Network Access and the fallbacks', () => {
    const r = explainBlock(new TransferError('mixed-content', 'blocked'), 'https://k-engine.org');
    const text = [r.title, ...r.reasons].join(' ');
    expect(text).toMatch(/mixed content/i);
    expect(text).toMatch(/Private Network Access/);
    const fb = r.fallbacks.join(' ');
    expect(fb).toMatch(/USB/);
    expect(fb).toMatch(/[Dd]ownload/);
    expect(fb).toMatch(/http:\/\/localhost/);
    expect(fb).toMatch(/helper/);
    expect(fb).toMatch(/CORS/);
    const n = explainBlock(new TransferError('network', 'x'), 'http://localhost:5173');
    expect(n.reasons.join(' ')).not.toMatch(/mixed content/i);
    for (const code of ['auth', 'exists', 'unsupported', 'cancelled', 'device', 'unknown', 'not-found'] as const) {
      const e = explainBlock(new TransferError(code, 'm'), 'https://k-engine.org');
      expect(e.title.length).toBeGreaterThan(0);
      expect(e.reasons.length).toBeGreaterThan(0);
      expect(e.fallbacks.length).toBeGreaterThan(0);
    }
  });
});

describe('webdav', () => {
  it('creates folders, uses PUT and verifies with HEAD', async () => {
    const file = outputFile();
    const fetches = stubFetch((c): MockResponseInit => {
      if (c.method === 'OPTIONS') return { status: 200, headers: { DAV: '1' } };
      if (c.method === 'MKCOL') return { status: c.url.endsWith('/Books/') ? 201 : 405 };
      if (c.method === 'HEAD') return { status: 200, headers: { 'Content-Length': String(file.blob.size) } };
      return { status: 500 };
    });
    const xhrs = stubXhr(() => ({ status: 201 }));
    const c = collector();
    const res = await getTransport('webdav').send(profile(), file, c.cb);

    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('/Books/K-rosspoint/My Book.epub');
    expect(res.bytes).toBe(file.blob.size);
    expect(fetches.map((f) => `${f.method} ${f.url}`)).toEqual([
      'OPTIONS http://192.168.1.50:80/',
      'MKCOL http://192.168.1.50:80/Books/',
      'MKCOL http://192.168.1.50:80/Books/K-rosspoint/',
      'HEAD http://192.168.1.50:80/Books/K-rosspoint/My%20Book.epub',
    ]);
    expect(xhrs).toHaveLength(1);
    expect(xhrs[0].method).toBe('PUT');
    expect(xhrs[0].url).toBe('http://192.168.1.50:80/Books/K-rosspoint/My%20Book.epub');
    expect(xhrs[0].body).toBe(file.blob);
    expect(xhrs[0].headers.Authorization).toBeUndefined();
    const states = c.states();
    for (const s of ['connecting', 'uploading', 'verifying', 'complete']) expect(states).toContain(s);
    expect(states.indexOf('verifying')).toBeGreaterThan(states.indexOf('uploading'));
    expect(c.events.some((e) => e.state === 'uploading' && e.fraction === 0.5)).toBe(true);
  });

  it('sends Basic auth only when a username is set', async () => {
    const fetches = stubFetch((c) => (c.method === 'HEAD' ? { status: 200 } : { status: 201 }));
    const xhrs = stubXhr(() => ({ status: 204 }));
    await getTransport('webdav').send(profile({ username: 'me', password: 'pw' }), outputFile(), () => {});
    expect(xhrs[0].headers.Authorization).toBe(`Basic ${btoa('me:pw')}`);
    expect(fetches[0].headers.Authorization).toBe(`Basic ${btoa('me:pw')}`);
  });

  it('fails verification on a size mismatch', async () => {
    stubFetch((c) => (c.method === 'HEAD' ? { status: 200, headers: { 'Content-Length': '3' } } : { status: 201 }));
    stubXhr(() => ({ status: 201 }));
    const err = await getTransport('webdav').send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('device');
  });

  it('maps 401 to auth', async () => {
    stubFetch(() => ({ status: 401 }));
    stubXhr(() => ({ status: 201 }));
    const err = await getTransport('webdav').send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err.code).toBe('auth');
  });

  it('aborts the XHR on cancel', async () => {
    stubFetch(() => ({ status: 201 }));
    stubXhr(() => ({ status: 201 }));
    const ctrl = new AbortController();
    const p = getTransport('webdav').send(profile(), outputFile(), (e) => {
      if (e.state === 'uploading') ctrl.abort();
    }, ctrl.signal);
    const err = await p.catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('cancelled');
  });

  it('testConnection uses OPTIONS and reports failure as a TransferError', async () => {
    const fetches = stubFetch(() => ({ status: 200, headers: { DAV: '1,2' } }));
    const ok = await getTransport('webdav').testConnection(profile());
    expect(ok.ok).toBe(true);
    expect(fetches[0].method).toBe('OPTIONS');
    stubFetch(() => ({ status: 404 }));
    const bad = await getTransport('webdav').testConnection(profile());
    expect(bad.ok).toBe(false);
    expect(bad.error?.code).toBe('not-found');
  });
});

describe('crosspoint-http', () => {
  it('uses a multipart POST to /upload and verifies with /api/files', async () => {
    const file = outputFile();
    const fetches = stubFetch((c): MockResponseInit => {
      if (c.url.includes('/api/files')) return { status: 200, body: JSON.stringify([{ name: file.name, size: file.blob.size, isDirectory: false, isEpub: true }]) };
      return { status: 200 };
    });
    const xhrs = stubXhr(() => ({ status: 200, body: `File uploaded successfully: ${file.name}` }));
    const c = collector();
    const res = await getTransport('crosspoint-http').send(profile({ method: 'crosspoint-http' }), file, c.cb);

    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('/Books/K-rosspoint/My Book.epub');
    const mkdirs = fetches.filter((f) => f.url.endsWith('/mkdir'));
    expect(mkdirs).toHaveLength(2);
    expect(String(mkdirs[0].body)).toBe('name=Books&path=%2F');
    expect(String(mkdirs[1].body)).toBe('name=K-rosspoint&path=%2FBooks');
    expect(xhrs).toHaveLength(1);
    expect(xhrs[0].method).toBe('POST');
    expect(xhrs[0].url).toBe('http://192.168.1.50:80/upload?path=%2FBooks%2FK-rosspoint');
    expect(xhrs[0].body).toBeInstanceOf(FormData);
    const part = (xhrs[0].body as FormData).get('file') as File;
    expect(part).toBeTruthy();
    expect(part.name).toBe(file.name);
    expect(part.size).toBe(file.blob.size);
    expect(fetches.at(-1)!.url).toBe('http://192.168.1.50:80/api/files?path=%2FBooks%2FK-rosspoint');
    expect(c.states()).toContain('verifying');
    expect(c.states().at(-1)).toBe('complete');
  });

  it('deletes and retries once when the file already exists', async () => {
    const file = outputFile();
    const fetches = stubFetch((c): MockResponseInit => {
      if (c.url.includes('/api/files')) return { status: 200, body: JSON.stringify([{ name: file.name, size: file.blob.size }]) };
      return { status: 200 };
    });
    let n = 0;
    const xhrs = stubXhr(() => (++n === 1 ? { status: 400, body: `File already exists: ${file.name}` } : { status: 200, body: 'ok' }));
    const res = await getTransport('crosspoint-http').send(profile({ method: 'crosspoint-http' }), file, () => {});
    expect(res.ok).toBe(true);
    expect(xhrs).toHaveLength(2);
    const del = fetches.find((f) => f.url.endsWith('/delete'));
    expect(del).toBeTruthy();
    expect(new URLSearchParams(String(del!.body)).get('paths')).toBe(JSON.stringify(['/Books/K-rosspoint/My Book.epub']));
  });

  it('rejects with exists when the retry also fails', async () => {
    stubFetch(() => ({ status: 200, body: '[]' }));
    stubXhr(() => ({ status: 400, body: 'File already exists: x' }));
    const err = await getTransport('crosspoint-http').send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('exists');
  });

  it('rejects when the file is missing after upload', async () => {
    stubFetch(() => ({ status: 200, body: '[]' }));
    stubXhr(() => ({ status: 200 }));
    const err = await getTransport('crosspoint-http').send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err.code).toBe('device');
  });

  it('testConnection reads /api/status into info', async () => {
    const status = { version: '1.2.0', ip: '192.168.1.50', mode: 'STA', rssi: -50, freeHeap: 1000, uptime: 5, device: 'X4' };
    const fetches = stubFetch(() => ({ status: 200, body: JSON.stringify(status) }));
    const r = await getTransport('crosspoint-http').testConnection(profile());
    expect(r.ok).toBe(true);
    expect(r.info).toEqual(status);
    expect(fetches[0].url).toBe('http://192.168.1.50:80/api/status');
    stubFetch(() => new TypeError('Failed to fetch'));
    const bad = await getTransport('crosspoint-http').testConnection(profile());
    expect(bad.ok).toBe(false);
    expect(bad.error).toBeInstanceOf(TransferError);
  });
});

describe('ble mock', () => {
  it('reaches complete with progress events', async () => {
    const c = collector();
    const t0 = Date.now();
    const res = await getTransport('ble').send(profile({ method: 'ble', firmware: 'kreader' }), outputFile('x'.repeat(5000)), c.cb);
    expect(Date.now() - t0).toBeGreaterThanOrEqual(1000);
    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('/Books/K-rosspoint/My Book.epub');
    const states = c.states();
    expect(states[0]).toBe('connecting');
    expect(states).toContain('uploading');
    expect(states).toContain('verifying');
    expect(states.at(-1)).toBe('complete');
    expect(c.events.filter((e) => e.state === 'uploading').length).toBeGreaterThanOrEqual(10);
  });

  it('can be cancelled', async () => {
    const ctrl = new AbortController();
    setTimeout(() => ctrl.abort(), 50);
    const err = await getTransport('ble').send(profile(), outputFile(), () => {}, ctrl.signal).catch((e) => e);
    expect(err.code).toBe('cancelled');
  });

  it('crc32 matches the IEEE check value', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
  });
});

describe('helper', () => {
  it('posts multipart to /send and returns the remote path', async () => {
    const h = getHelperTransport();
    h.setHelperUrl('http://127.0.0.1:8134/');
    const file = outputFile();
    const xhrs = stubXhr(() => ({ status: 200, body: JSON.stringify({ ok: true, remotePath: '/Books/K-rosspoint/My Book.epub', bytes: file.blob.size }) }));
    const c = collector();
    const res = await h.send(profile({ method: 'crosspoint-http' }), file, c.cb);
    expect(res.ok).toBe(true);
    expect(xhrs[0].url).toBe('http://127.0.0.1:8134/send');
    const fd = xhrs[0].body as FormData;
    expect(fd.get('host')).toBe('192.168.1.50');
    expect(fd.get('port')).toBe('80');
    expect(fd.get('folder')).toBe('/Books/K-rosspoint/');
    expect(fd.get('method')).toBe('crosspoint-http');
    expect((fd.get('file') as File).name).toBe(file.name);
    expect(c.states().at(-1)).toBe('complete');
    h.setHelperUrl(undefined);
  });

  it('explains when the helper does not run', async () => {
    const h = getHelperTransport();
    h.setHelperUrl('http://127.0.0.1:8134');
    stubXhr(() => 'network-error');
    const err = await h.send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('network');
    expect(err.message).toMatch(/helper/);
    stubXhr(() => ({ status: 502, body: JSON.stringify({ ok: false, error: 'device unreachable', code: 'not-found' }) }));
    const err2 = await h.send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err2.code).toBe('not-found');
    h.setHelperUrl(undefined);
  });

  it('testConnection calls /status and /test', async () => {
    const h = getHelperTransport();
    h.setHelperUrl('http://127.0.0.1:8134');
    const fetches = stubFetch((c) =>
      c.url.endsWith('/status')
        ? { status: 200, body: JSON.stringify({ ok: true, version: '0.1.0' }) }
        : { status: 200, body: JSON.stringify({ ok: true, info: { version: '1.0' }, latencyMs: 4 }) },
    );
    const r = await h.testConnection(profile());
    expect(r.ok).toBe(true);
    expect(fetches.map((f) => f.method)).toEqual(['GET', 'POST']);
    expect(JSON.parse(String(fetches[1].body))).toEqual({ host: '192.168.1.50', port: 80 });
    h.setHelperUrl(undefined);
  });
});
