/**
 * Bluetooth transport: the CrossPoint BLE file transfer protocol, version 1,
 * over Web Bluetooth. The binding contract is docs/BLE_PROTOCOL.md. This file
 * implements the "Sender behaviour" section of that document.
 *
 * Layers:
 * - BleLink: a thin GATT link (connect, read INFO, write META / CONTROL with
 *   response, write CHUNK without response, STATUS notifications). The
 *   default link is WebBluetoothLink. Tests inject a fake link.
 * - BleTransport: the protocol logic (META, READY, chunk window, ACK, VERIFY,
 *   COMMIT, ABORT, timeouts, size rules, AbortSignal).
 *
 * User gesture rule: navigator.bluetooth.requestDevice() works only while the
 * page handles a click (transient user activation). The design:
 * 1. The "Connect" button (testConnection) is the click that chooses the
 *    reader. The chosen BluetoothDevice is cached in memory per profile id,
 *    and its name and id go into the profile (onBleDeviceChosen).
 * 2. send() uses the cached device. When the page was reloaded, send() looks
 *    for the stored device id in navigator.bluetooth.getDevices() (no prompt).
 * 3. When neither finds a device, send() calls requestDevice() itself. That
 *    works when send() runs from the Send button click and the click is still
 *    fresh. When the browser refuses (no user activation), send() fails with
 *    TransferError 'not-paired': "press Connect first".
 */
import {
  TransferError,
  type ConnectionTestResult,
  type DeviceProfile,
  type OutputFile,
  type TransferProgress,
  type TransferResult,
  type TransportAdapter,
} from '../types';
import { blobToArrayBuffer, normalizeFolder, remoteFilePath, runSend, safeEmitter } from './common';
import { crc32 } from './crc32';

// ───────────── Protocol constants (docs/BLE_PROTOCOL.md) ─────────────

const UUID = (xx: string): string => `6b726472-${xx}-4b52-8000-00000000f11e`;
export const BLE_SERVICE_UUID = UUID('0001');
export const BLE_META_UUID = UUID('0002');
export const BLE_CHUNK_UUID = UUID('0003');
export const BLE_CONTROL_UUID = UUID('0004');
export const BLE_STATUS_UUID = UUID('0005');
export const BLE_INFO_UUID = UUID('0006');

export const BLE_PROTOCOL_VERSION = 1;
export const BLE_DEFAULT_FOLDER = '/Books/K-rosspoint/';
/** At most this many chunks past the last ACK. */
export const BLE_WINDOW_CHUNKS = 48;
/** The reader ACKs after every this many chunks. */
export const BLE_ACK_EVERY = 16;
/** Senders refuse files over 16 MB. */
export const BLE_MAX_BYTES = 16 * 1024 * 1024;
/** Senders warn, but allow, files over 3 MB. */
export const BLE_WARN_BYTES = 3 * 1024 * 1024;
export const BLE_READY_TIMEOUT_MS = 15000;
export const BLE_ACK_TIMEOUT_MS = 10000;
export const BLE_CONNECT_TIMEOUT_MS = 15000;
const ABORT_WRITE_TIMEOUT_MS = 2000;

export const BleOp = { VERIFY: 0x02, COMMIT: 0x03, ABORT: 0x04, PING: 0x05 } as const;

export const BleStatusCode = {
  READY: 0x00,
  ACK: 0x01,
  CRC_OK: 0x02,
  CRC_BAD: 0x03,
  COMMITTED: 0x04,
  ERROR: 0x05,
  BUSY: 0x06,
  NO_SPACE: 0x07,
  ABORTED: 0x08,
  PONG: 0x09,
} as const;

export interface BleStatus {
  code: number;
  value: number;
  text: string;
}

export interface BleInfo {
  protocolVersion: number;
  maxChunkData: number;
  freeKiB: number;
  firmware: string;
}

// ───────────── Encoding ─────────────

const enc = new TextEncoder();
const dec = new TextDecoder();

/** STATUS: u8 code | u32 value | text. Undefined when shorter than 5 bytes. */
export function parseStatus(data: Uint8Array): BleStatus | undefined {
  if (data.length < 5) return undefined;
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  return { code: data[0], value: dv.getUint32(1, true), text: dec.decode(data.subarray(5)) };
}

/** Builds a STATUS packet. Used by tests and fakes. */
export function encodeStatus(code: number, value = 0, text = ''): Uint8Array {
  const t = enc.encode(text);
  const out = new Uint8Array(5 + t.length);
  out[0] = code;
  new DataView(out.buffer).setUint32(1, value >>> 0, true);
  out.set(t, 5);
  return out;
}

/** INFO: u8 version | u16 maxChunkData | u32 freeKiB | u8 nameLen | name. */
export function parseInfo(data: Uint8Array): BleInfo {
  if (data.length < 8) throw new TransferError('device', `The reader sent a short INFO record (${data.length} bytes).`);
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const nameLen = data[7];
  return {
    protocolVersion: data[0],
    maxChunkData: dv.getUint16(1, true),
    freeKiB: dv.getUint32(3, true),
    firmware: dec.decode(data.subarray(8, 8 + nameLen)),
  };
}

/** Checks a file name against the reader's rules. Returns a reason or undefined. */
function badName(name: string): string | undefined {
  if (!name) return 'The file name is empty.';
  if (/[\\/]/.test(name)) return `The file name "${name}" contains a slash.`;
  if (name === '.' || name === '..' || name.startsWith('.')) return `The file name "${name}" starts with a dot.`;
  return undefined;
}

/**
 * META: u8 version | u32 fileSize | u32 crc32 | u8 nameLen | name | u8 pathLen | path.
 * Throws a TransferError when the name or the path breaks the rules.
 */
export function encodeMeta(fileSize: number, crc: number, name: string, path: string): Uint8Array {
  const why = badName(name);
  if (why) throw new TransferError('device', `${why} The reader refuses it. Rename the file.`);
  const n = enc.encode(name);
  const p = enc.encode(path);
  if (n.length < 1 || n.length > 200) {
    throw new TransferError('device', `The file name is ${n.length} bytes. Bluetooth transfer allows 1 to 200. Use a shorter name.`);
  }
  if (!path.startsWith('/') || p.length < 1 || p.length > 200) {
    throw new TransferError('device', 'The target folder must start with "/" and be at most 200 bytes.');
  }
  const out = new Uint8Array(1 + 4 + 4 + 1 + n.length + 1 + p.length);
  const dv = new DataView(out.buffer);
  out[0] = BLE_PROTOCOL_VERSION;
  dv.setUint32(1, fileSize >>> 0, true);
  dv.setUint32(5, crc >>> 0, true);
  out[9] = n.length;
  out.set(n, 10);
  out[10 + n.length] = p.length;
  out.set(p, 11 + n.length);
  return out;
}

/** CHUNK: u32 offset | data. */
export function encodeChunk(offset: number, data: Uint8Array): Uint8Array {
  const out = new Uint8Array(4 + data.length);
  new DataView(out.buffer).setUint32(0, offset >>> 0, true);
  out.set(data, 4);
  return out;
}

// ───────────── Link ─────────────

/** A GATT link to one reader. The protocol logic talks only to this. */
export interface BleLink {
  /** Chooses (or reuses) a device that offers serviceUuid and connects to it. */
  connect(serviceUuid: string): Promise<{ name: string; id: string }>;
  /** Reads INFO. */
  readInfo(): Promise<Uint8Array>;
  /** Writes META with response. */
  writeMeta(data: Uint8Array): Promise<void>;
  /** Writes CONTROL with response. */
  writeControl(data: Uint8Array): Promise<void>;
  /** Writes CHUNK without response. */
  writeChunk(data: Uint8Array): Promise<void>;
  /** Starts STATUS notifications. */
  subscribeStatus(onStatus: (data: Uint8Array) => void): Promise<void>;
  /** Called once when the link drops. */
  onDisconnect(cb: () => void): void;
  readonly connected: boolean;
  disconnect(): void;
}

export type BleLinkFactory = (profile: DeviceProfile) => BleLink;

// Minimal Web Bluetooth types (TypeScript's DOM lib has none).
interface BtCharacteristic extends EventTarget {
  value?: DataView | null;
  readValue(): Promise<DataView>;
  writeValueWithResponse(v: BufferSource): Promise<void>;
  writeValueWithoutResponse(v: BufferSource): Promise<void>;
  startNotifications(): Promise<BtCharacteristic>;
}
interface BtService {
  getCharacteristic(uuid: string): Promise<BtCharacteristic>;
}
interface BtServer {
  readonly connected: boolean;
  connect(): Promise<BtServer>;
  disconnect(): void;
  getPrimaryService(uuid: string): Promise<BtService>;
}
export interface BtDevice extends EventTarget {
  readonly id: string;
  readonly name?: string;
  readonly gatt?: BtServer;
  watchAdvertisements?(options?: { signal?: AbortSignal }): Promise<void>;
}
interface BtApi {
  requestDevice(options: { filters: Array<{ services: string[] }>; optionalServices?: string[] }): Promise<BtDevice>;
  getDevices?(): Promise<BtDevice[]>;
}

function bluetoothApi(): BtApi | undefined {
  if (typeof navigator === 'undefined') return undefined;
  return (navigator as Navigator & { bluetooth?: BtApi }).bluetooth;
}

/** Chosen devices, per profile id. In memory only: a BluetoothDevice cannot be stored. */
const deviceCache = new Map<string, BtDevice>();

type ChosenListener = (profileId: string, device: { name: string; id: string }) => void;
const chosenListeners = new Set<ChosenListener>();

/**
 * Registers a callback for "the user chose a reader for this profile".
 * The UI uses it to store bleDeviceName and bleDeviceId in the profile.
 * Returns an unsubscribe function.
 */
export function onBleDeviceChosen(cb: ChosenListener): () => void {
  chosenListeners.add(cb);
  return () => chosenListeners.delete(cb);
}

function notifyChosen(profileId: string, device: { name: string; id: string }): void {
  for (const cb of chosenListeners) {
    try {
      cb(profileId, device);
    } catch {
      /* A listener bug must not break the transfer. */
    }
  }
}

/** Forgets the cached device of a profile. The next Connect asks again. */
export function forgetBleDevice(profileId: string): void {
  deviceCache.delete(profileId);
}

/** True when a device is cached in memory for this profile. */
export function hasCachedBleDevice(profileId: string): boolean {
  return deviceCache.has(profileId);
}

/** Maps a Web Bluetooth exception to a TransferError. */
export function bleError(err: unknown, during = 'The Bluetooth transfer'): TransferError {
  if (err instanceof TransferError) return err;
  const name = (err as { name?: string } | null)?.name;
  const msg = (err as { message?: string } | null)?.message ?? String(err);
  switch (name) {
    case 'AbortError':
      return new TransferError('cancelled', 'The transfer was cancelled.', err);
    case 'NotFoundError':
      return new TransferError('cancelled', 'No reader was chosen. On the reader open File Transfer > Receive via Bluetooth, then press Connect.', err);
    case 'SecurityError':
    case 'NotAllowedError':
      return notPaired(err);
    case 'NotSupportedError':
      return new TransferError('unsupported', `${during} failed: the browser or the reader does not support this operation (${msg}).`, err);
    case 'NetworkError':
    case 'InvalidStateError':
      return new TransferError(
        'network',
        `${during} failed: the Bluetooth link to the reader broke (${msg}). Keep the reader on Receive via Bluetooth and near this computer.`,
        err,
      );
    default:
      return new TransferError('unknown', `${during} failed: ${msg}`, err);
  }
}

function notPaired(cause?: unknown): TransferError {
  return new TransferError(
    'not-paired',
    'Choose the reader first: on the reader open File Transfer > Receive via Bluetooth, then press Connect on this device. ' +
      'The browser shows the Bluetooth chooser only after a click.',
    cause,
  );
}

function withTimeout<T>(p: Promise<T>, ms: number, onTimeout: () => TransferError): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const t = setTimeout(() => reject(onTimeout()), ms);
    p.then(
      (v) => {
        clearTimeout(t);
        resolve(v);
      },
      (e) => {
        clearTimeout(t);
        reject(e);
      },
    );
  });
}

function cancelled(): TransferError {
  return new TransferError('cancelled', 'The transfer was cancelled.');
}

/** Rejects with 'cancelled' when the signal aborts first. */
function raceSignal<T>(p: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (!signal) return p;
  if (signal.aborted) return Promise.reject(cancelled());
  return new Promise<T>((resolve, reject) => {
    const onAbort = () => reject(cancelled());
    signal.addEventListener('abort', onAbort, { once: true });
    p.then(
      (v) => {
        signal.removeEventListener('abort', onAbort);
        resolve(v);
      },
      (e) => {
        signal.removeEventListener('abort', onAbort);
        reject(e);
      },
    );
  });
}

/** The default link: Web Bluetooth. One instance per connection. */
export class WebBluetoothLink implements BleLink {
  private device?: BtDevice;
  private server?: BtServer;
  private meta?: BtCharacteristic;
  private chunk?: BtCharacteristic;
  private control?: BtCharacteristic;
  private status?: BtCharacteristic;
  private info?: BtCharacteristic;
  private disconnectCb?: () => void;
  private statusHandler?: (ev: Event) => void;
  private closed = false;
  private readonly onGattDisconnected = () => {
    if (this.closed) return;
    this.closed = true;
    this.disconnectCb?.();
  };

  constructor(
    private readonly profile: Pick<DeviceProfile, 'id' | 'bleDeviceId'>,
    private readonly connectTimeoutMs = BLE_CONNECT_TIMEOUT_MS,
  ) {}

  get connected(): boolean {
    return !this.closed && !!this.server?.connected;
  }

  private async findDevice(bt: BtApi, serviceUuid: string): Promise<{ dev: BtDevice; chosen: boolean }> {
    const cached = deviceCache.get(this.profile.id);
    if (cached) return { dev: cached, chosen: false };
    if (this.profile.bleDeviceId && typeof bt.getDevices === 'function') {
      try {
        const known = await bt.getDevices();
        const dev = known.find((d) => d.id === this.profile.bleDeviceId);
        if (dev) return { dev, chosen: false };
      } catch {
        /* getDevices can be off (flag) or fail. Fall back to the chooser. */
      }
    }
    try {
      const dev = await bt.requestDevice({ filters: [{ services: [serviceUuid] }] });
      return { dev, chosen: true };
    } catch (err) {
      throw bleError(err, 'Choosing the reader');
    }
  }

  private async gattConnect(dev: BtDevice): Promise<BtServer> {
    if (!dev.gatt) throw new TransferError('unsupported', 'This Bluetooth device has no GATT server.');
    const label = dev.name || 'the reader';
    const timeoutErr = () =>
      new TransferError(
        'not-found',
        `Could not connect to ${label} within ${this.connectTimeoutMs / 1000} s. On the reader open File Transfer > Receive via Bluetooth.`,
      );
    try {
      return await withTimeout(dev.gatt.connect(), this.connectTimeoutMs, timeoutErr);
    } catch (first) {
      // A device from getDevices() may need to be seen advertising first.
      if (typeof dev.watchAdvertisements !== 'function' || first instanceof TransferError) throw bleError(first, 'Connecting');
      const ctrl = new AbortController();
      try {
        const seen = new Promise<void>((resolve) => dev.addEventListener('advertisementreceived', () => resolve(), { once: true }));
        await dev.watchAdvertisements({ signal: ctrl.signal });
        await withTimeout(seen, 5000, timeoutErr);
      } catch (err) {
        throw bleError(err, 'Connecting');
      } finally {
        ctrl.abort();
      }
      return await withTimeout(dev.gatt.connect(), this.connectTimeoutMs, timeoutErr);
    }
  }

  async connect(serviceUuid: string): Promise<{ name: string; id: string }> {
    const bt = bluetoothApi();
    if (!bt) throw new TransferError('unsupported', 'This browser has no Web Bluetooth. Use Chrome or Edge.');
    const { dev, chosen } = await this.findDevice(bt, serviceUuid);
    this.device = dev;
    deviceCache.set(this.profile.id, dev);
    const ident = { name: dev.name || 'CrossPoint', id: dev.id };
    if (chosen || dev.id !== this.profile.bleDeviceId) notifyChosen(this.profile.id, ident);
    dev.addEventListener('gattserverdisconnected', this.onGattDisconnected);
    try {
      this.server = await this.gattConnect(dev);
      const svc = await this.server.getPrimaryService(serviceUuid);
      [this.meta, this.chunk, this.control, this.status, this.info] = await Promise.all([
        svc.getCharacteristic(BLE_META_UUID),
        svc.getCharacteristic(BLE_CHUNK_UUID),
        svc.getCharacteristic(BLE_CONTROL_UUID),
        svc.getCharacteristic(BLE_STATUS_UUID),
        svc.getCharacteristic(BLE_INFO_UUID),
      ]);
    } catch (err) {
      this.disconnect();
      if (err instanceof TransferError) throw err;
      if ((err as { name?: string } | null)?.name === 'NotFoundError') {
        throw new TransferError(
          'device',
          `${ident.name} does not offer the CrossPoint file service. Install the ble-transfer firmware build and open Receive via Bluetooth.`,
          err,
        );
      }
      throw bleError(err, 'Connecting');
    }
    return ident;
  }

  private need(c: BtCharacteristic | undefined): BtCharacteristic {
    if (!c || this.closed) throw new TransferError('network', 'The Bluetooth link to the reader is closed.');
    return c;
  }

  async readInfo(): Promise<Uint8Array> {
    const dv = await this.need(this.info).readValue();
    return new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength));
  }

  async writeMeta(data: Uint8Array): Promise<void> {
    await this.need(this.meta).writeValueWithResponse(data as Uint8Array<ArrayBuffer>);
  }

  async writeControl(data: Uint8Array): Promise<void> {
    await this.need(this.control).writeValueWithResponse(data as Uint8Array<ArrayBuffer>);
  }

  async writeChunk(data: Uint8Array): Promise<void> {
    await this.need(this.chunk).writeValueWithoutResponse(data as Uint8Array<ArrayBuffer>);
  }

  async subscribeStatus(onStatus: (data: Uint8Array) => void): Promise<void> {
    const ch = this.need(this.status);
    this.statusHandler = (ev: Event) => {
      const dv = (ev.target as BtCharacteristic).value;
      if (dv) onStatus(new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength)));
    };
    ch.addEventListener('characteristicvaluechanged', this.statusHandler);
    await ch.startNotifications();
  }

  onDisconnect(cb: () => void): void {
    this.disconnectCb = cb;
  }

  disconnect(): void {
    this.closed = true;
    if (this.status && this.statusHandler) this.status.removeEventListener('characteristicvaluechanged', this.statusHandler);
    this.device?.removeEventListener('gattserverdisconnected', this.onGattDisconnected);
    try {
      if (this.server?.connected) this.server.disconnect();
    } catch {
      /* Already gone. */
    }
  }
}

// ───────────── STATUS channel ─────────────

interface Waiter {
  pred: (s: BleStatus) => boolean;
  resolve: (s: BleStatus) => void;
  reject: (e: TransferError) => void;
  timer?: ReturnType<typeof setTimeout>;
}

const STATUS_NAMES: Record<number, string> = Object.fromEntries(Object.entries(BleStatusCode).map(([k, v]) => [v, k]));

/** Turns a STATUS that ends the session into a TransferError. */
function statusError(s: BleStatus): TransferError {
  switch (s.code) {
    case BleStatusCode.ERROR:
      return new TransferError('device', `The reader reported an error: ${s.text || 'no reason given'}.`);
    case BleStatusCode.BUSY:
      return new TransferError('device', 'The reader is busy with another transfer. Wait and send again.');
    case BleStatusCode.NO_SPACE:
      return new TransferError(
        'device',
        `The SD card on the reader is full (${s.value} KiB free). Delete some files and send again.`,
      );
    case BleStatusCode.ABORTED:
      return new TransferError('device', `The reader aborted the transfer after ${s.value} bytes.`);
    default:
      return new TransferError('device', `The reader sent an unexpected status ${STATUS_NAMES[s.code] ?? s.code}.`);
  }
}

/**
 * Collects STATUS notifications. ERROR, BUSY, NO_SPACE and an unasked ABORTED
 * fail every wait at once. ACK updates lastAck.
 */
class StatusChannel {
  lastAck = 0;
  private waiters = new Set<Waiter>();
  private fatal?: TransferError;
  private ackWaiters = new Set<() => void>();
  /** True after this side sent ABORT: ABORTED is then expected, not fatal. */
  aborting = false;

  constructor(private readonly onAck?: (offset: number) => void) {}

  push(raw: Uint8Array): void {
    const s = parseStatus(raw);
    if (!s) return;
    if (s.code === BleStatusCode.ACK) {
      if (s.value > this.lastAck) {
        this.lastAck = s.value;
        this.onAck?.(s.value);
      }
      for (const w of this.ackWaiters) w();
    }
    if (
      s.code === BleStatusCode.ERROR ||
      s.code === BleStatusCode.BUSY ||
      s.code === BleStatusCode.NO_SPACE ||
      (s.code === BleStatusCode.ABORTED && !this.aborting)
    ) {
      this.fail(statusError(s));
      return;
    }
    for (const w of Array.from(this.waiters)) {
      if (w.pred(s)) {
        this.drop(w);
        w.resolve(s);
      }
    }
  }

  fail(err: TransferError): void {
    if (this.fatal) return;
    this.fatal = err;
    for (const w of Array.from(this.waiters)) {
      this.drop(w);
      w.reject(err);
    }
    for (const w of this.ackWaiters) w();
  }

  /** Throws the stored failure, if any. */
  check(): void {
    if (this.fatal) throw this.fatal;
  }

  private drop(w: Waiter): void {
    if (w.timer) clearTimeout(w.timer);
    this.waiters.delete(w);
  }

  /**
   * Waits for a status that matches pred. Register it before the write that
   * causes the status, so a fast reply is not missed.
   */
  expect(pred: (s: BleStatus) => boolean, ms: number, onTimeout: () => TransferError): Promise<BleStatus> {
    const p = new Promise<BleStatus>((resolve, reject) => {
      if (this.fatal) {
        reject(this.fatal);
        return;
      }
      const w: Waiter = { pred, resolve, reject };
      w.timer = setTimeout(() => {
        this.drop(w);
        reject(onTimeout());
      }, ms);
      this.waiters.add(w);
    });
    p.catch(() => undefined); // The caller awaits it; this only stops an unhandled-rejection report.
    return p;
  }

  /**
   * Waits until lastAck >= min. The timeout restarts on every ACK that moves
   * lastAck forward, so it means "no ACK within ms".
   */
  waitAck(min: number, ms: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      let timer: ReturnType<typeof setTimeout> | undefined;
      let seen = this.lastAck;
      const done = (err?: TransferError) => {
        if (timer) clearTimeout(timer);
        this.ackWaiters.delete(check);
        if (err) reject(err);
        else resolve();
      };
      const arm = () => {
        if (timer) clearTimeout(timer);
        timer = setTimeout(
          () =>
            done(
              new TransferError(
                'network',
                `The reader sent no ACK within ${ms / 1000} s (at ${this.lastAck} bytes). Keep the reader on Receive via Bluetooth and near this computer.`,
              ),
            ),
          ms,
        );
      };
      const check = () => {
        if (this.fatal) return done(this.fatal);
        if (this.lastAck >= min) return done();
        if (this.lastAck > seen) {
          seen = this.lastAck;
          arm();
        }
      };
      if (this.fatal) return reject(this.fatal);
      if (this.lastAck >= min) return resolve();
      this.ackWaiters.add(check);
      arm();
    });
  }

  close(): void {
    for (const w of Array.from(this.waiters)) this.drop(w);
    this.ackWaiters.clear();
  }
}

// ───────────── Transport ─────────────

export interface BleTransportOptions {
  /** Makes the link for a profile. Default: a WebBluetoothLink. */
  linkFactory?: BleLinkFactory;
  readyTimeoutMs?: number;
  ackTimeoutMs?: number;
  connectTimeoutMs?: number;
}

function mb(bytes: number): string {
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export class BleTransport implements TransportAdapter {
  readonly method = 'ble' as const;
  private readonly linkFactory: BleLinkFactory;
  private readonly readyTimeoutMs: number;
  private readonly ackTimeoutMs: number;

  constructor(opts: BleTransportOptions = {}) {
    const connectMs = opts.connectTimeoutMs ?? BLE_CONNECT_TIMEOUT_MS;
    this.linkFactory = opts.linkFactory ?? ((p) => new WebBluetoothLink(p, connectMs));
    this.readyTimeoutMs = opts.readyTimeoutMs ?? BLE_READY_TIMEOUT_MS;
    this.ackTimeoutMs = opts.ackTimeoutMs ?? BLE_ACK_TIMEOUT_MS;
  }

  isSupported(): boolean {
    return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
  }

  limitations(): string[] {
    const out: string[] = [];
    if (!this.isSupported()) {
      out.push('This browser has no Web Bluetooth. Use Chrome or Edge (desktop or Android). Firefox and Safari do not have it.');
    }
    out.push(
      'Needs Chrome or Edge. Works from https and from a copy of this page opened from your disk. Wi-Fi and mixed-content limits do not apply.',
      'Needs the CrossPoint ble-transfer firmware build. On the reader open File Transfer > Receive via Bluetooth, then press Connect here.',
      'Speed is about 10 to 50 KB/s: good for articles, slow for packs. Files over 3 MB show a warning. Files over 16 MB are refused: use Wi-Fi or USB.',
    );
    return out;
  }

  async testConnection(profile: DeviceProfile): Promise<ConnectionTestResult> {
    const t0 = Date.now();
    let link: BleLink | undefined;
    const ch = new StatusChannel();
    try {
      link = this.linkFactory(profile);
      link.onDisconnect(() => ch.fail(new TransferError('network', 'The reader disconnected.')));
      const dev = await link.connect(BLE_SERVICE_UUID);
      await link.subscribeStatus((d) => ch.push(d));
      const info = parseInfo(await link.readInfo());
      if (info.protocolVersion !== BLE_PROTOCOL_VERSION) {
        throw new TransferError(
          'device',
          `The reader speaks Bluetooth transfer protocol version ${info.protocolVersion}. This app speaks version ${BLE_PROTOCOL_VERSION}. Update the app or the firmware.`,
        );
      }
      const pong = ch.expect(
        (s) => s.code === BleStatusCode.PONG,
        this.readyTimeoutMs,
        () => new TransferError('device', `${dev.name} did not answer PING within ${this.readyTimeoutMs / 1000} s.`),
      );
      await link.writeControl(Uint8Array.of(BleOp.PING));
      await pong;
      return {
        ok: true,
        latencyMs: Date.now() - t0,
        info: {
          bleDeviceName: dev.name,
          bleDeviceId: dev.id,
          name: dev.name,
          version: info.firmware,
          protocolVersion: info.protocolVersion,
          maxChunkData: info.maxChunkData,
          freeKiB: info.freeKiB,
        },
      };
    } catch (err) {
      return { ok: false, error: bleError(err, 'The Bluetooth connection test') };
    } finally {
      ch.close();
      link?.disconnect();
    }
  }

  send(
    profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    return runSend(file, onProgress, undefined, async () => {
      const total = file.blob.size;
      // Size rules come first: refuse before any radio work.
      if (total > BLE_MAX_BYTES) {
        throw new TransferError(
          'too-large',
          `${file.name} is ${mb(total)}. Bluetooth transfer is limited to 16 MB (it would take ten minutes or more). Send it over Wi-Fi or USB.`,
        );
      }
      if (total === 0) throw new TransferError('device', `${file.name} is empty. Nothing to send.`);
      const folder = profile.targetFolder ? normalizeFolder(profile.targetFolder) : BLE_DEFAULT_FOLDER;
      if (signal?.aborted) throw cancelled();

      const bytes = new Uint8Array(await blobToArrayBuffer(file.blob));
      const crc = crc32(bytes);
      const meta = encodeMeta(total, crc, file.name, folder);

      if (total > BLE_WARN_BYTES) {
        const minutes = Math.ceil(total / (20 * 1024) / 60);
        emit({
          state: 'connecting',
          message: `Warning: ${file.name} is ${mb(total)}. Bluetooth sends about 10 to 50 KB/s, so this can take ${minutes} minutes or more. Wi-Fi or USB is faster.`,
        });
      }

      const report = (acked: number) =>
        emit({
          state: 'uploading',
          fraction: Math.min(1, acked / total),
          bytesSent: acked,
          bytesTotal: total,
          message: 'Bluetooth',
        });
      const ch = new StatusChannel(report);
      const link = this.linkFactory(profile);
      link.onDisconnect(() =>
        ch.fail(new TransferError('network', 'The reader disconnected. Keep it on Receive via Bluetooth and send again.')),
      );
      const onAbort = () => ch.fail(cancelled());
      signal?.addEventListener('abort', onAbort, { once: true });
      let sessionOpen = false;

      try {
        emit({ state: 'connecting', message: profile.bleDeviceName ? `Connecting to ${profile.bleDeviceName}` : 'Connecting over Bluetooth' });
        const dev = await raceSignal(link.connect(BLE_SERVICE_UUID), signal);
        ch.check();
        await raceSignal(link.subscribeStatus((d) => ch.push(d)), signal);

        // INFO is optional: a failed read does not stop the send.
        let info: BleInfo | undefined;
        try {
          info = parseInfo(await raceSignal(link.readInfo(), signal));
        } catch {
          if (signal?.aborted) throw cancelled();
        }
        if (info && info.freeKiB > 0 && info.freeKiB * 1024 < total) {
          throw new TransferError(
            'device',
            `The SD card on ${dev.name} has ${info.freeKiB} KiB free. ${file.name} needs ${Math.ceil(total / 1024)} KiB.`,
          );
        }
        ch.check();

        emit({ state: 'connecting', message: `Starting the transfer to ${dev.name}` });
        const ready = ch.expect(
          (s) => s.code === BleStatusCode.READY,
          this.readyTimeoutMs,
          () =>
            new TransferError(
              'device',
              `${dev.name} did not answer READY within ${this.readyTimeoutMs / 1000} s. Check that the reader still shows Receive via Bluetooth.`,
            ),
        );
        sessionOpen = true;
        await link.writeMeta(meta);
        const chunkSize = (await ready).value;
        if (!Number.isInteger(chunkSize) || chunkSize < 1 || chunkSize > 512) {
          throw new TransferError('device', `The reader asked for a chunk size of ${chunkSize} bytes. The protocol allows 16 to 512.`);
        }
        report(0);

        // Stream. Never more than BLE_WINDOW_CHUNKS chunks past the last ACK.
        let sent = 0;
        while (sent < total) {
          const len = Math.min(chunkSize, total - sent);
          const minAck = sent - (BLE_WINDOW_CHUNKS - 1) * chunkSize;
          if (ch.lastAck < minAck) await ch.waitAck(minAck, this.ackTimeoutMs);
          ch.check();
          await link.writeChunk(encodeChunk(sent, bytes.subarray(sent, sent + len)));
          sent += len;
        }
        await ch.waitAck(total, this.ackTimeoutMs);

        emit({ state: 'verifying', fraction: 1, bytesSent: total, bytesTotal: total, message: `Checking CRC-32 ${crc.toString(16).padStart(8, '0')}` });
        const verified = ch.expect(
          (s) => s.code === BleStatusCode.CRC_OK || s.code === BleStatusCode.CRC_BAD,
          this.ackTimeoutMs,
          () => new TransferError('device', `${dev.name} did not answer VERIFY within ${this.ackTimeoutMs / 1000} s.`),
        );
        await link.writeControl(Uint8Array.of(BleOp.VERIFY));
        const v = await verified;
        if (v.code === BleStatusCode.CRC_BAD || v.value >>> 0 !== crc) {
          throw new TransferError(
            'device',
            `The file arrived damaged: CRC-32 ${(v.value >>> 0).toString(16).padStart(8, '0')} on the reader, ${crc.toString(16).padStart(8, '0')} here. Send it again.`,
          );
        }

        emit({ state: 'verifying', fraction: 1, bytesSent: total, bytesTotal: total, message: `Saving in ${folder}` });
        const committed = ch.expect(
          (s) => s.code === BleStatusCode.COMMITTED,
          this.ackTimeoutMs,
          () => new TransferError('device', `${dev.name} did not answer COMMIT within ${this.ackTimeoutMs / 1000} s.`),
        );
        await link.writeControl(Uint8Array.of(BleOp.COMMIT));
        const c = await committed;
        sessionOpen = false;
        return c.text || remoteFilePath(folder, file.name);
      } catch (err) {
        const te = bleError(err);
        if (sessionOpen && link.connected) {
          ch.aborting = true;
          try {
            await withTimeout(link.writeControl(Uint8Array.of(BleOp.ABORT)), ABORT_WRITE_TIMEOUT_MS, () => cancelled());
          } catch {
            /* Best effort: the reader also deletes the partial file on disconnect. */
          }
        }
        throw te;
      } finally {
        signal?.removeEventListener('abort', onAbort);
        ch.close();
        link.disconnect();
      }
    });
  }
}

export const bleTransport = new BleTransport();
