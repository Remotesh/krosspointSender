/**
 * CrossPoint BLE file transfer protocol (docs/BLE_PROTOCOL.md): protocol
 * logic against a fake reader link, plus the Web Bluetooth link against a
 * fake navigator.bluetooth.
 */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { TransferError, type OutputFile, type TransferProgress } from '../../src/types';
import {
  BLE_CHUNK_UUID,
  BLE_CONTROL_UUID,
  BLE_INFO_UUID,
  BLE_META_UUID,
  BLE_SERVICE_UUID,
  BLE_STATUS_UUID,
  BleOp,
  BleStatusCode,
  BleTransport,
  WebBluetoothLink,
  bleError,
  encodeMeta,
  encodeStatus,
  forgetBleDevice,
  onBleDeviceChosen,
  parseInfo,
  parseStatus,
  type BleLink,
} from '../../src/transports/ble';
import { crc32, crc32Update } from '../../src/transports/crc32';
import { explainBlock } from '../../src/transports/explain';
import { validateProfile } from '../../src/deviceProfiles';
import { profile } from './mocks';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

const dec = new TextDecoder();

interface FakeOpts {
  /** maxChunkData in READY. */
  chunk?: number;
  /** What META does. */
  onMeta?: 'ready' | 'none' | 'no-space' | 'busy';
  /** Delay before each STATUS notification, ms. */
  delayMs?: number;
  /** Stop sending ACKs once this many bytes were received. */
  stopAckAt?: number;
  /** Send ERROR "Out of order" when this chunk index arrives. */
  errorAtChunk?: number;
  /** Reply CRC_BAD to VERIFY. */
  crcBad?: boolean;
  /** Do not answer PING. */
  noPong?: boolean;
  freeKiB?: number;
}

/** A fake reader that follows the protocol, with knobs for failures. */
class FakeReader implements BleLink {
  connected = false;
  statusCb?: (d: Uint8Array) => void;
  disconnectCb?: () => void;
  meta?: { version: number; size: number; crc: number; name: string; path: string };
  data: number[] = [];
  chunkLens: number[] = [];
  controls: number[] = [];
  infoReads = 0;
  ackDelivered = 0;
  maxOutstanding = 0;
  private chunks = 0;
  private sinceAck = 0;
  private dead = false;

  constructor(private readonly o: FakeOpts = {}) {}

  private get chunk(): number {
    return this.o.chunk ?? 240;
  }

  private emit(code: number, value = 0, text = ''): void {
    setTimeout(() => {
      if (!this.connected) return;
      if (code === BleStatusCode.ACK) this.ackDelivered = value;
      this.statusCb?.(encodeStatus(code, value, text));
    }, this.o.delayMs ?? 0);
  }

  async connect(uuid: string) {
    expect(uuid).toBe(BLE_SERVICE_UUID);
    this.connected = true;
    return { name: 'CrossPoint-3A7F', id: 'dev-abc' };
  }

  async readInfo(): Promise<Uint8Array> {
    this.infoReads++;
    const name = new TextEncoder().encode('1.6.5-x4pro');
    const out = new Uint8Array(8 + name.length);
    const dv = new DataView(out.buffer);
    out[0] = 1;
    dv.setUint16(1, this.chunk, true);
    dv.setUint32(3, this.o.freeKiB ?? 0, true);
    out[7] = name.length;
    out.set(name, 8);
    return out;
  }

  async writeMeta(d: Uint8Array): Promise<void> {
    const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const nl = d[9];
    const name = dec.decode(d.subarray(10, 10 + nl));
    const pl = d[10 + nl];
    const path = dec.decode(d.subarray(11 + nl, 11 + nl + pl));
    this.meta = { version: d[0], size: dv.getUint32(1, true), crc: dv.getUint32(5, true), name, path };
    const mode = this.o.onMeta ?? 'ready';
    if (mode === 'ready') this.emit(BleStatusCode.READY, this.chunk);
    else if (mode === 'no-space') this.emit(BleStatusCode.NO_SPACE, 12);
    else if (mode === 'busy') this.emit(BleStatusCode.BUSY);
  }

  async writeChunk(d: Uint8Array): Promise<void> {
    if (this.dead) return;
    const offset = new DataView(d.buffer, d.byteOffset, d.byteLength).getUint32(0, true);
    const payload = d.subarray(4);
    this.chunkLens.push(payload.length);
    if (this.o.errorAtChunk !== undefined && this.chunks === this.o.errorAtChunk) {
      this.dead = true;
      this.emit(BleStatusCode.ERROR, 0, 'Out of order');
      return;
    }
    if (offset !== this.data.length) {
      this.dead = true;
      this.emit(BleStatusCode.ERROR, 0, 'Out of order');
      return;
    }
    this.data.push(...payload);
    this.chunks++;
    this.sinceAck++;
    const outstanding = Math.ceil((this.data.length - this.ackDelivered) / this.chunk);
    this.maxOutstanding = Math.max(this.maxOutstanding, outstanding);
    const done = this.data.length === this.meta!.size;
    const stopped = this.o.stopAckAt !== undefined && this.data.length > this.o.stopAckAt;
    if (!stopped && (this.sinceAck === 16 || done)) {
      this.sinceAck = 0;
      this.emit(BleStatusCode.ACK, this.data.length);
    }
  }

  async writeControl(d: Uint8Array): Promise<void> {
    const op = d[0];
    this.controls.push(op);
    const got = crc32(Uint8Array.from(this.data));
    switch (op) {
      case BleOp.VERIFY:
        this.emit(this.o.crcBad ? BleStatusCode.CRC_BAD : BleStatusCode.CRC_OK, this.o.crcBad ? got ^ 1 : got);
        break;
      case BleOp.COMMIT:
        this.emit(BleStatusCode.COMMITTED, this.data.length, `${this.meta!.path}${this.meta!.name}`);
        break;
      case BleOp.ABORT:
        this.emit(BleStatusCode.ABORTED, this.data.length);
        break;
      case BleOp.PING:
        if (!this.o.noPong) this.emit(BleStatusCode.PONG, 1);
        break;
    }
  }

  async subscribeStatus(cb: (d: Uint8Array) => void): Promise<void> {
    this.statusCb = cb;
  }

  onDisconnect(cb: () => void): void {
    this.disconnectCb = cb;
  }

  disconnect(): void {
    this.connected = false;
  }

  /** Simulates the reader dropping the link. */
  drop(): void {
    this.connected = false;
    this.disconnectCb?.();
  }
}

function bytesFile(n: number, name = 'My Book.epub'): OutputFile {
  const b = new Uint8Array(n);
  for (let i = 0; i < n; i++) b[i] = (i * 31 + 7) & 0xff;
  return { name, mime: 'application/epub+zip', blob: new Blob([b]), format: 'epub' };
}

function setup(o: FakeOpts = {}, timeouts = { readyTimeoutMs: 80, ackTimeoutMs: 80 }) {
  const reader = new FakeReader(o);
  const factory = vi.fn(() => reader);
  const t = new BleTransport({ linkFactory: factory, ...timeouts });
  const events: TransferProgress[] = [];
  const p = profile({ method: 'ble', host: '', firmware: 'crosspoint' });
  return { reader, factory, t, events, p, cb: (e: TransferProgress) => events.push(e) };
}

describe('crc32', () => {
  it('matches the spec test vector', () => {
    expect(crc32(new TextEncoder().encode('123456789'))).toBe(0xcbf43926);
  });
  it('can run in parts', () => {
    const a = new TextEncoder().encode('12345');
    const b = new TextEncoder().encode('6789');
    expect(crc32Update(crc32Update(0, a), b)).toBe(0xcbf43926);
  });
});

describe('ble encoding', () => {
  it('encodes META little-endian with name and path', () => {
    const m = encodeMeta(0x01020304, 0xcbf43926, 'a.epub', '/Books/');
    expect(Array.from(m.subarray(0, 10))).toEqual([1, 4, 3, 2, 1, 0x26, 0x39, 0xf4, 0xcb, 6]);
    expect(dec.decode(m.subarray(10, 16))).toBe('a.epub');
    expect(m[16]).toBe(7);
    expect(dec.decode(m.subarray(17))).toBe('/Books/');
  });
  it('refuses bad names before any radio work', () => {
    expect(() => encodeMeta(1, 0, 'a/b.epub', '/')).toThrow(TransferError);
    expect(() => encodeMeta(1, 0, '.hidden', '/')).toThrow(TransferError);
    expect(() => encodeMeta(1, 0, 'x'.repeat(201), '/')).toThrow(/1 to 200/);
  });
  it('parses STATUS and INFO', () => {
    expect(parseStatus(encodeStatus(BleStatusCode.COMMITTED, 99, '/a.epub'))).toEqual({ code: 4, value: 99, text: '/a.epub' });
    expect(parseStatus(new Uint8Array(3))).toBeUndefined();
    const info = new Uint8Array([1, 240, 0, 0x10, 0, 0, 0, 2, 0x68, 0x69]);
    expect(parseInfo(info)).toEqual({ protocolVersion: 1, maxChunkData: 240, freeKiB: 16, firmware: 'hi' });
  });
});

describe('BleTransport.send', () => {
  it('happy path: META, chunks, VERIFY, COMMIT, remote path from COMMITTED', async () => {
    const { reader, t, events, p, cb } = setup();
    const file = bytesFile(10_000);
    const res = await t.send(p, file, cb);
    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('/Books/K-rosspoint/My Book.epub');
    expect(reader.infoReads).toBe(1);
    expect(reader.meta).toMatchObject({ version: 1, size: 10_000, name: 'My Book.epub', path: '/Books/K-rosspoint/' });
    const sent = new Uint8Array(await file.blob.arrayBuffer());
    expect(reader.meta!.crc).toBe(crc32(sent));
    expect(Uint8Array.from(reader.data)).toEqual(sent);
    expect(reader.controls).toEqual([BleOp.VERIFY, BleOp.COMMIT]);
    const states = events.map((e) => e.state);
    expect(states[0]).toBe('connecting');
    expect(states).toContain('uploading');
    expect(states).toContain('verifying');
    expect(states.at(-1)).toBe('complete');
    const fr = events.filter((e) => e.state === 'uploading').map((e) => e.fraction!);
    for (let i = 1; i < fr.length; i++) expect(fr[i]).toBeGreaterThanOrEqual(fr[i - 1]);
    expect(fr.at(-1)).toBe(1);
    expect(reader.connected).toBe(false);
  });

  it('uses the chunk size from READY', async () => {
    const { reader, t, p, cb } = setup({ chunk: 100 });
    await t.send(p, bytesFile(1050), cb);
    expect(reader.chunkLens.slice(0, -1).every((n) => n === 100)).toBe(true);
    expect(reader.chunkLens.at(-1)).toBe(50);
    expect(reader.chunkLens.length).toBe(11);
  });

  it('never has more than 48 chunks outstanding past the last ACK', async () => {
    const { reader, t, p, cb } = setup({ chunk: 20, delayMs: 2 }, { readyTimeoutMs: 500, ackTimeoutMs: 500 });
    await t.send(p, bytesFile(20 * 300 + 7), cb);
    expect(reader.maxOutstanding).toBeLessThanOrEqual(48);
    expect(reader.maxOutstanding).toBeGreaterThan(16); // the window is really used
    expect(reader.data.length).toBe(20 * 300 + 7);
  });

  it('fails on ERROR "Out of order" and sends ABORT', async () => {
    const { reader, t, p, events, cb } = setup({ chunk: 20, errorAtChunk: 5, delayMs: 1 });
    const err = await t.send(p, bytesFile(20 * 200), cb).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('device');
    expect(err.message).toMatch(/Out of order/);
    expect(reader.controls).toContain(BleOp.ABORT);
    expect(reader.controls).not.toContain(BleOp.COMMIT);
    expect(events.at(-1)?.state).toBe('failed');
  });

  it('CRC_BAD: sends ABORT, never COMMIT', async () => {
    const { reader, t, p, cb } = setup({ crcBad: true });
    const err = await t.send(p, bytesFile(500), cb).catch((e) => e);
    expect(err.code).toBe('device');
    expect(err.message).toMatch(/damaged/);
    expect(reader.controls).toEqual([BleOp.VERIFY, BleOp.ABORT]);
  });

  it('NO_SPACE after META fails with a clear message', async () => {
    const { reader, t, p, cb } = setup({ onMeta: 'no-space' });
    const err = await t.send(p, bytesFile(500), cb).catch((e) => e);
    expect(err.code).toBe('device');
    expect(err.message).toMatch(/full/);
    expect(reader.chunkLens.length).toBe(0);
  });

  it('READY timeout', async () => {
    const { reader, t, p, cb } = setup({ onMeta: 'none' });
    const err = await t.send(p, bytesFile(500), cb).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.message).toMatch(/READY within/);
    expect(reader.controls).toEqual([BleOp.ABORT]);
  });

  it('ACK timeout', async () => {
    const { reader, t, p, cb } = setup({ chunk: 20, stopAckAt: 0 });
    const err = await t.send(p, bytesFile(20 * 100), cb).catch((e) => e);
    expect(err.code).toBe('network');
    expect(err.message).toMatch(/no ACK/);
    expect(reader.chunkLens.length).toBe(48); // stopped at the window
    expect(reader.controls).toEqual([BleOp.ABORT]);
  });

  it('a dropped link fails the send', async () => {
    const { reader, t, p, cb } = setup({ chunk: 20, stopAckAt: 0 }, { readyTimeoutMs: 500, ackTimeoutMs: 5000 });
    setTimeout(() => reader.drop(), 30);
    const err = await t.send(p, bytesFile(20 * 100), cb).catch((e) => e);
    expect(err.code).toBe('network');
    expect(err.message).toMatch(/disconnected/);
    expect(reader.controls).not.toContain(BleOp.ABORT); // link is down
  });

  it('honours the abort signal and sends ABORT', async () => {
    const { reader, t, p, cb } = setup({ chunk: 20, delayMs: 3 }, { readyTimeoutMs: 500, ackTimeoutMs: 500 });
    const ctrl = new AbortController();
    setTimeout(() => ctrl.abort(), 30);
    const err = await t.send(p, bytesFile(20 * 2000), cb, ctrl.signal).catch((e) => e);
    expect(err.code).toBe('cancelled');
    expect(reader.controls).toContain(BleOp.ABORT);
    expect(reader.data.length).toBeLessThan(20 * 2000);
  });

  it('an already aborted signal never connects', async () => {
    const { factory, t, p, cb } = setup();
    const ctrl = new AbortController();
    ctrl.abort();
    const err = await t.send(p, bytesFile(10), cb, ctrl.signal).catch((e) => e);
    expect(err.code).toBe('cancelled');
    expect(factory).not.toHaveBeenCalled();
  });

  it('refuses files over 16 MB before connecting', async () => {
    const { factory, t, p, cb } = setup();
    const big: OutputFile = { name: 'pack.epub', mime: 'application/epub+zip', blob: new Blob([new Uint8Array(16 * 1024 * 1024 + 1)]), format: 'epub' };
    const err = await t.send(p, big, cb).catch((e) => e);
    expect(err.code).toBe('too-large');
    expect(err.message).toMatch(/Wi-Fi/);
    expect(factory).not.toHaveBeenCalled();
    const ex = explainBlock(err, 'https://k-engine.org', 'ble');
    expect(ex.fallbacks.join(' ')).toMatch(/Wi-Fi/);
  });

  it('warns over 3 MB but goes on', async () => {
    const { reader, t, p, events, cb } = setup({ onMeta: 'busy' });
    const err = await t.send(p, bytesFile(3 * 1024 * 1024 + 1), cb).catch((e) => e);
    expect(events.some((e) => /Warning: .* 3\.0 MB/.test(e.message ?? ''))).toBe(true);
    expect(reader.meta?.size).toBe(3 * 1024 * 1024 + 1); // it was allowed to start
    expect(err.message).toMatch(/busy/);
  });
});

describe('BleTransport.testConnection', () => {
  it('reads INFO, pings, and reports the device', async () => {
    const { t, p } = setup({ freeKiB: 2048 });
    const r = await t.testConnection(p);
    expect(r.ok).toBe(true);
    expect(r.info).toMatchObject({ bleDeviceName: 'CrossPoint-3A7F', bleDeviceId: 'dev-abc', version: '1.6.5-x4pro', freeKiB: 2048, protocolVersion: 1 });
  });
  it('fails when PING gets no PONG', async () => {
    const { t, p } = setup({ noPong: true });
    const r = await t.testConnection(p);
    expect(r.ok).toBe(false);
    expect(r.error).toBeInstanceOf(TransferError);
  });
  it('limitations and support detection', () => {
    const t = new BleTransport();
    expect(t.limitations().join(' ')).toMatch(/Chrome or Edge/);
    expect(t.limitations().join(' ')).toMatch(/16 MB/);
    expect(t.isSupported()).toBe('bluetooth' in navigator);
  });
});

describe('ble errors and profiles', () => {
  it('maps a no-gesture SecurityError to not-paired', () => {
    const e = bleError(Object.assign(new Error('Must be handling a user gesture'), { name: 'SecurityError' }));
    expect(e.code).toBe('not-paired');
    expect(e.message).toMatch(/Connect/);
    expect(explainBlock(e, 'https://k-engine.org').title).toMatch(/Choose the reader/);
  });
  it('Bluetooth explanations skip the mixed-content text', () => {
    const e = new TransferError('network', 'link broke');
    const ex = explainBlock(e, 'https://k-engine.org', 'ble');
    expect(ex.reasons.join(' ')).not.toMatch(/mixed content/);
    expect(ex.reasons.join(' ')).toMatch(/Receive via Bluetooth/);
  });
  it('a ble profile needs no host and keeps the device fields', () => {
    expect(validateProfile(profile({ method: 'ble', host: '', bleDeviceId: 'abc', bleDeviceName: 'CrossPoint-3A7F' }))).toEqual([]);
  });
});

// ───────────── Web Bluetooth link against a fake navigator.bluetooth ─────────────

function fakeBluetooth() {
  const writes: Array<{ uuid: string; data: number[]; response: boolean }> = [];
  const chars = new Map<string, EventTarget & Record<string, unknown>>();
  let statusChar: (EventTarget & { value?: DataView }) | undefined;
  const notify = (bytes: Uint8Array) => {
    statusChar!.value = new DataView(bytes.buffer);
    statusChar!.dispatchEvent(new Event('characteristicvaluechanged'));
  };
  const mkChar = (uuid: string) => {
    const c = new EventTarget() as EventTarget & Record<string, unknown>;
    c.readValue = async () => new DataView(new Uint8Array([1, 240, 0, 0, 4, 0, 0, 1, 0x76]).buffer);
    c.writeValueWithResponse = async (d: Uint8Array) => {
      writes.push({ uuid, data: Array.from(d), response: true });
      if (uuid === BLE_CONTROL_UUID && d[0] === BleOp.PING) setTimeout(() => notify(encodeStatus(BleStatusCode.PONG, 1)), 0);
    };
    c.writeValueWithoutResponse = async (d: Uint8Array) => void writes.push({ uuid, data: Array.from(d), response: false });
    c.startNotifications = async () => c;
    if (uuid === BLE_STATUS_UUID) statusChar = c as EventTarget & { value?: DataView };
    chars.set(uuid, c);
    return c;
  };
  for (const u of [BLE_META_UUID, BLE_CHUNK_UUID, BLE_CONTROL_UUID, BLE_STATUS_UUID, BLE_INFO_UUID]) mkChar(u);
  const server = {
    connected: false,
    async connect() {
      server.connected = true;
      return server;
    },
    disconnect() {
      server.connected = false;
    },
    async getPrimaryService(uuid: string) {
      expect(uuid).toBe(BLE_SERVICE_UUID);
      return { getCharacteristic: async (u: string) => chars.get(u)! };
    },
  };
  const device = Object.assign(new EventTarget(), { id: 'web-id-1', name: 'CrossPoint-3A7F', gatt: server });
  const bluetooth = {
    requestDevice: vi.fn(async (_opts: unknown) => device),
    getDevices: vi.fn(async () => [device]),
  };
  vi.stubGlobal('navigator', { ...navigator, bluetooth });
  return { bluetooth, server, writes };
}

describe('WebBluetoothLink', () => {
  it('asks for the device by service UUID, caches it, and reports the choice', async () => {
    const fb = fakeBluetooth();
    const chosen: string[] = [];
    const off = onBleDeviceChosen((pid, d) => chosen.push(`${pid}:${d.name}:${d.id}`));
    const t = new BleTransport();
    const p = profile({ id: 'p-web', method: 'ble', host: '' });
    forgetBleDevice(p.id);
    const r = await t.testConnection(p);
    expect(r.ok).toBe(true);
    expect(fb.bluetooth.requestDevice).toHaveBeenCalledWith({ filters: [{ services: [BLE_SERVICE_UUID] }] });
    expect(chosen).toEqual(['p-web:CrossPoint-3A7F:web-id-1']);
    expect(fb.writes.some((w) => w.uuid === BLE_CONTROL_UUID && w.data[0] === BleOp.PING && w.response)).toBe(true);
    expect(fb.server.connected).toBe(false); // the test disconnects
    // Second use: the cached device, no prompt.
    await t.testConnection({ ...p, bleDeviceId: 'web-id-1' });
    expect(fb.bluetooth.requestDevice).toHaveBeenCalledTimes(1);
    off();
    forgetBleDevice(p.id);
  });

  it('reconnects through getDevices() without a prompt after a reload', async () => {
    const fb = fakeBluetooth();
    const link = new WebBluetoothLink({ id: 'p-reload', bleDeviceId: 'web-id-1' });
    forgetBleDevice('p-reload');
    const dev = await link.connect(BLE_SERVICE_UUID);
    expect(dev).toEqual({ name: 'CrossPoint-3A7F', id: 'web-id-1' });
    expect(fb.bluetooth.getDevices).toHaveBeenCalled();
    expect(fb.bluetooth.requestDevice).not.toHaveBeenCalled();
    await link.writeChunk(Uint8Array.of(0, 0, 0, 0, 9));
    expect(fb.writes.at(-1)).toEqual({ uuid: BLE_CHUNK_UUID, data: [0, 0, 0, 0, 9], response: false });
    link.disconnect();
    expect(link.connected).toBe(false);
    forgetBleDevice('p-reload');
  });

  it('send without a gesture fails with not-paired', async () => {
    const fb = fakeBluetooth();
    fb.bluetooth.requestDevice.mockRejectedValueOnce(Object.assign(new Error('Must be handling a user gesture to show a permission request.'), { name: 'SecurityError' }));
    fb.bluetooth.getDevices.mockResolvedValueOnce([]);
    const t = new BleTransport();
    const p = profile({ id: 'p-nogesture', method: 'ble', host: '' });
    forgetBleDevice(p.id);
    const err = await t.send(p, bytesFile(100), () => {}).catch((e) => e);
    expect(err.code).toBe('not-paired');
  });

  it('reports no Web Bluetooth as unsupported', async () => {
    const nav = { ...navigator } as Record<string, unknown>;
    delete nav.bluetooth;
    vi.stubGlobal('navigator', nav);
    const r = await new BleTransport().testConnection(profile({ id: 'p-none', method: 'ble', host: '' }));
    expect(r.error?.code).toBe('unsupported');
  });
});
