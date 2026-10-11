/**
 * Test doubles for fetch, XMLHttpRequest and WebSocket.
 */
import { vi } from 'vitest';
import type { DeviceProfile, OutputFile } from '../../src/types';

export interface MockResponseInit {
  status?: number;
  body?: string;
  headers?: Record<string, string>;
}

export function mockResponse(init: MockResponseInit = {}) {
  const status = init.status ?? 200;
  const body = init.body ?? '';
  const headers = Object.fromEntries(Object.entries(init.headers ?? {}).map(([k, v]) => [k.toLowerCase(), v]));
  return {
    ok: status >= 200 && status < 300,
    status,
    headers: { get: (n: string) => headers[n.toLowerCase()] ?? null },
    text: async () => body,
    json: async () => JSON.parse(body),
  };
}

export interface FetchCall {
  url: string;
  method: string;
  body: unknown;
  headers: Record<string, string>;
}

/** Stubs fetch with a handler. Returns the call log. */
export function stubFetch(handler: (call: FetchCall) => MockResponseInit | Error) {
  const calls: FetchCall[] = [];
  const fn = vi.fn(async (url: string, init: RequestInit = {}) => {
    const call: FetchCall = {
      url: String(url),
      method: (init.method ?? 'GET').toUpperCase(),
      body: init.body,
      headers: { ...((init.headers as Record<string, string>) ?? {}) },
    };
    calls.push(call);
    const r = handler(call);
    if (r instanceof Error) throw r;
    return mockResponse(r);
  });
  vi.stubGlobal('fetch', fn);
  return calls;
}

export interface XhrCall {
  method: string;
  url: string;
  body: unknown;
  headers: Record<string, string>;
}

export type XhrReply = { status: number; body?: string; headers?: Record<string, string> } | 'network-error';

/** Stubs XMLHttpRequest. Returns the call log. */
export function stubXhr(handler: (call: XhrCall) => XhrReply) {
  const calls: XhrCall[] = [];
  class FakeXhr {
    upload: { onprogress: ((ev: ProgressEvent) => void) | null } = { onprogress: null };
    onload: (() => void) | null = null;
    onerror: (() => void) | null = null;
    onabort: (() => void) | null = null;
    status = 0;
    responseText = '';
    private method = '';
    private url = '';
    private reqHeaders: Record<string, string> = {};
    private resHeaders: Record<string, string> = {};
    private aborted = false;
    open(method: string, url: string) {
      this.method = method;
      this.url = url;
    }
    setRequestHeader(k: string, v: string) {
      this.reqHeaders[k] = v;
    }
    getResponseHeader(n: string) {
      return this.resHeaders[n.toLowerCase()] ?? null;
    }
    abort() {
      this.aborted = true;
      this.onabort?.();
    }
    send(body: unknown) {
      const call: XhrCall = { method: this.method, url: this.url, body, headers: { ...this.reqHeaders } };
      calls.push(call);
      setTimeout(() => {
        if (this.aborted) return;
        const reply = handler(call);
        if (reply === 'network-error') {
          this.onerror?.();
          return;
        }
        const size = body instanceof Blob ? body.size : 100;
        this.upload.onprogress?.({ lengthComputable: true, loaded: Math.floor(size / 2), total: size } as ProgressEvent);
        this.upload.onprogress?.({ lengthComputable: true, loaded: size, total: size } as ProgressEvent);
        this.status = reply.status;
        this.responseText = reply.body ?? '';
        this.resHeaders = Object.fromEntries(Object.entries(reply.headers ?? {}).map(([k, v]) => [k.toLowerCase(), v]));
        this.onload?.();
      }, 0);
    }
  }
  vi.stubGlobal('XMLHttpRequest', FakeXhr);
  return calls;
}

export function profile(over: Partial<DeviceProfile> = {}): DeviceProfile {
  return {
    id: 'dev-1',
    name: 'Test reader',
    firmware: 'crosspoint',
    host: '192.168.1.50',
    port: 80,
    method: 'webdav',
    targetFolder: '/Books/K-rosspoint/',
    defaultFormat: 'epub',
    createdAt: '2026-10-10T00:00:00.000Z',
    updatedAt: '2026-10-10T00:00:00.000Z',
    ...over,
  };
}

export function outputFile(content = 'hello epub bytes', name = 'My Book.epub'): OutputFile {
  return {
    name,
    mime: 'application/epub+zip',
    blob: new Blob([content], { type: 'application/epub+zip' }),
    format: 'epub',
  };
}
