import { afterEach, describe, expect, it, vi } from 'vitest';
import { TransferError, type TransferProgress } from '../../src/types';
import { getTransport, WS_CHUNK_SIZE } from '../../src/transports';
import { outputFile, profile } from './mocks';

type Script = (ws: FakeWs, msg: string | ArrayBuffer) => void;

class FakeWs {
  static last: FakeWs | null = null;
  static script: Script = () => {};
  static failOpen = false;
  url: string;
  binaryType = 'blob';
  bufferedAmount = 0;
  sent: (string | ArrayBuffer)[] = [];
  onopen: (() => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onerror: (() => void) | null = null;
  onclose: ((ev: { code: number }) => void) | null = null;
  closed = false;
  constructor(url: string) {
    this.url = url;
    FakeWs.last = this;
    setTimeout(() => (FakeWs.failOpen ? this.onerror?.() : this.onopen?.()), 0);
  }
  send(data: string | ArrayBuffer) {
    this.sent.push(data);
    // Simulate a send buffer that drains a little later.
    if (typeof data !== 'string') {
      this.bufferedAmount += data.byteLength;
      setTimeout(() => (this.bufferedAmount = Math.max(0, this.bufferedAmount - data.byteLength)), 1);
    }
    setTimeout(() => FakeWs.script(this, data), 0);
  }
  reply(text: string) {
    this.onmessage?.({ data: text });
  }
  close() {
    this.closed = true;
  }
}

afterEach(() => {
  vi.unstubAllGlobals();
  FakeWs.failOpen = false;
});

describe('crosspoint-ws', () => {
  it('sends START, waits for READY, streams 16 KiB chunks and ends on DONE', async () => {
    vi.stubGlobal('WebSocket', FakeWs);
    const size = WS_CHUNK_SIZE * 5 + 123;
    let received = 0;
    FakeWs.script = (ws, msg) => {
      if (typeof msg === 'string') {
        if (msg.startsWith('START:')) ws.reply('READY');
        return;
      }
      received += msg.byteLength;
      ws.reply(`PROGRESS:${received}:${size}`);
      if (received >= size) ws.reply('DONE');
    };
    const file = outputFile('a'.repeat(size));
    const events: TransferProgress[] = [];
    const res = await getTransport('crosspoint-ws').send(profile({ method: 'crosspoint-ws' }), file, (p) => events.push(p));

    const ws = FakeWs.last!;
    expect(ws.url).toBe('ws://192.168.1.50:81/');
    expect(ws.binaryType).toBe('arraybuffer');
    expect(ws.sent[0]).toBe(`START:My Book.epub:${size}:/Books/K-rosspoint`);
    const chunks = ws.sent.slice(1) as ArrayBuffer[];
    expect(chunks).toHaveLength(6);
    expect(chunks.every((c) => c instanceof ArrayBuffer && c.byteLength <= WS_CHUNK_SIZE)).toBe(true);
    expect(chunks.reduce((n, c) => n + c.byteLength, 0)).toBe(size);
    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('/Books/K-rosspoint/My Book.epub');
    expect(events.some((e) => e.state === 'uploading' && e.bytesSent === WS_CHUNK_SIZE)).toBe(true);
    expect(events.map((e) => e.state)).toContain('verifying');
    expect(events.at(-1)!.state).toBe('complete');
    expect(ws.closed).toBe(true);
  });

  it('rejects with device on ERROR', async () => {
    vi.stubGlobal('WebSocket', FakeWs);
    FakeWs.script = (ws, msg) => {
      if (typeof msg === 'string') ws.reply('READY');
      else ws.reply('ERROR:SD card full');
    };
    const err = await getTransport('crosspoint-ws').send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('device');
    expect(err.message).toMatch(/SD card full/);
  });

  it('rejects with network when the socket fails to open', async () => {
    vi.stubGlobal('WebSocket', FakeWs);
    FakeWs.failOpen = true;
    const err = await getTransport('crosspoint-ws').send(profile(), outputFile(), () => {}).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('network');
  });

  it('testConnection resolves ok when the socket opens', async () => {
    vi.stubGlobal('WebSocket', FakeWs);
    const r = await getTransport('crosspoint-ws').testConnection(profile());
    expect(r.ok).toBe(true);
  });
});
