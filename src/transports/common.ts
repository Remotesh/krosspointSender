/**
 * Shared helpers for the transport adapters.
 * - URL and path building.
 * - Error mapping to TransferError codes.
 * - fetch with a 5 s connect timeout (small requests).
 * - XMLHttpRequest uploads with real upload progress and no timeout.
 */
import {
  TransferError,
  type DeviceProfile,
  type OutputFile,
  type TransferErrorCode,
  type TransferProgress,
  type TransferResult,
} from '../types';

/** Connect timeout for small requests, in milliseconds. */
export const CONNECT_TIMEOUT_MS = 5000;

// ───────────── Origin and URL helpers ─────────────

/** The page protocol, e.g. "https:". Empty when there is no location (tests, workers). */
export function pageProtocol(): string {
  try {
    return globalThis.location?.protocol ?? '';
  } catch {
    return '';
  }
}

/** The page origin, e.g. "https://k-engine.org". */
export function pageOrigin(): string {
  try {
    return globalThis.location?.origin ?? '';
  } catch {
    return '';
  }
}

/** True for localhost names and loopback addresses. Browsers treat them as secure. */
export function isLoopbackHost(host: string): boolean {
  const h = host.toLowerCase().replace(/^\[|\]$/g, '');
  return h === 'localhost' || h.endsWith('.localhost') || h === '::1' || /^127\.\d+\.\d+\.\d+$/.test(h);
}

/** True when an http request to this host from this page is mixed content. */
export function isMixedContent(host: string, protocol: string = pageProtocol()): boolean {
  return protocol === 'https:' && !isLoopbackHost(host);
}

/** Base URL of the device, http://host:port. */
export function baseUrl(profile: Pick<DeviceProfile, 'host' | 'port'>): string {
  return `http://${profile.host}:${profile.port}`;
}

/** Splits a folder into clean path segments. "/Books//Inbox/" -> ["Books", "Inbox"]. */
export function folderSegments(folder: string): string[] {
  return (folder || '/')
    .split(/[\\/]+/)
    .map((s) => s.trim())
    .filter((s) => s !== '' && s !== '.' && s !== '..');
}

/** Normal folder form with a leading and a trailing slash. "/" for root. */
export function normalizeFolder(folder: string): string {
  const segs = folderSegments(folder);
  return segs.length === 0 ? '/' : `/${segs.join('/')}/`;
}

/** Folder without the trailing slash, as CrossPoint's ?path= wants it. "/" for root. */
export function folderNoTrailingSlash(folder: string): string {
  const segs = folderSegments(folder);
  return segs.length === 0 ? '/' : `/${segs.join('/')}`;
}

/** Remote path of a file in a folder. */
export function remoteFilePath(folder: string, name: string): string {
  return `${normalizeFolder(folder)}${name}`;
}

/** Percent-encodes each segment of an absolute path. */
export function encodePath(path: string): string {
  const trailing = path.endsWith('/');
  const segs = path.split('/').filter((s) => s !== '');
  const enc = '/' + segs.map(encodeURIComponent).join('/');
  return trailing && segs.length > 0 ? `${enc}/` : enc;
}

/** HTTP Basic auth header value. UTF-8 safe. Undefined when no username is set. */
export function basicAuthHeader(profile: Pick<DeviceProfile, 'username' | 'password'>): string | undefined {
  if (!profile.username) return undefined;
  const bytes = new TextEncoder().encode(`${profile.username}:${profile.password ?? ''}`);
  let bin = '';
  for (const b of bytes) bin += String.fromCharCode(b);
  return `Basic ${btoa(bin)}`;
}

// ───────────── Errors ─────────────

/** Converts any thrown value to a TransferError. Keeps a TransferError as it is. */
export function toTransferError(err: unknown, host?: string, protocol: string = pageProtocol()): TransferError {
  if (err instanceof TransferError) return err;
  const name = (err as { name?: string } | null)?.name;
  const msg = (err as { message?: string } | null)?.message ?? String(err);
  if (name === 'AbortError') return new TransferError('cancelled', 'The transfer was cancelled.', err);
  if (name === 'TimeoutError') {
    return new TransferError('not-found', `The device did not answer within ${CONNECT_TIMEOUT_MS / 1000} s.`, err);
  }
  if (name === 'SecurityError' && host && isMixedContent(host, protocol)) {
    return mixedContentError(host, err);
  }
  if (err instanceof TypeError || name === 'NetworkError') {
    return networkError(host, protocol, err);
  }
  return new TransferError('unknown', msg || 'An unknown error occurred.', err);
}

export function mixedContentError(host: string, cause?: unknown): TransferError {
  return new TransferError(
    'mixed-content',
    `The browser blocked the request to http://${host}. This page uses https, and the device uses plain http (mixed content).`,
    cause,
  );
}

/** A failed network request. On an https page to an http host this is mixed content. */
export function networkError(host: string | undefined, protocol: string = pageProtocol(), cause?: unknown): TransferError {
  if (host && isMixedContent(host, protocol)) return mixedContentError(host, cause);
  return new TransferError(
    'network',
    host
      ? `Could not reach ${host}. Make sure the device shows the File Transfer screen and is on the same Wi-Fi.`
      : 'The network request failed.',
    cause,
  );
}

/** Maps an HTTP status to an error code. */
export function statusCode(status: number): TransferErrorCode {
  if (status === 401 || status === 403) return 'auth';
  if (status === 404) return 'not-found';
  return 'device';
}

/** A TransferError for a bad HTTP status. */
export function httpError(status: number, what: string, body?: string): TransferError {
  const code = statusCode(status);
  const detail = body ? `: ${body.trim().slice(0, 200)}` : '';
  const text =
    code === 'auth'
      ? `${what} was refused (HTTP ${status}). Check the username and the password.`
      : code === 'not-found'
        ? `${what} failed: not found (HTTP 404)${detail}.`
        : `${what} failed: the device returned HTTP ${status}${detail}.`;
  return new TransferError(code, text);
}

// ───────────── fetch with timeout ─────────────

export interface SmallResponse {
  ok: boolean;
  status: number;
  headers: { get(name: string): string | null };
  text(): Promise<string>;
  json(): Promise<unknown>;
}

/**
 * fetch with a connect timeout. The timeout and the caller's signal both abort.
 * Network errors become TransferErrors. HTTP errors are returned, not thrown.
 */
export async function fetchWithTimeout(
  url: string,
  init: RequestInit & { host?: string; timeoutMs?: number } = {},
  signal?: AbortSignal,
): Promise<SmallResponse> {
  const { host, timeoutMs = CONNECT_TIMEOUT_MS, ...rest } = init;
  if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
  const ctrl = new AbortController();
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    ctrl.abort();
  }, timeoutMs);
  const onAbort = () => ctrl.abort();
  signal?.addEventListener('abort', onAbort, { once: true });
  try {
    return (await fetch(url, { ...rest, signal: ctrl.signal })) as SmallResponse;
  } catch (err) {
    if (timedOut) {
      throw new TransferError(
        'not-found',
        `${host ?? 'The server'} did not answer within ${timeoutMs / 1000} s. Check the address and that the device is awake.`,
        err,
      );
    }
    if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.', err);
    throw toTransferError(err, host);
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', onAbort);
  }
}

// ───────────── XMLHttpRequest upload ─────────────

export interface XhrResult {
  status: number;
  responseText: string;
  getResponseHeader(name: string): string | null;
}

export interface XhrOptions {
  method: string;
  url: string;
  body?: XMLHttpRequestBodyInit | null;
  headers?: Record<string, string>;
  host?: string;
  signal?: AbortSignal;
  onUploadProgress?: (loaded: number, total: number) => void;
}

/**
 * Sends one request with XMLHttpRequest. No timeout (uploads can be slow).
 * Resolves with any HTTP status. Rejects with a TransferError on a network
 * error or an abort.
 */
export function xhrRequest(opts: XhrOptions): Promise<XhrResult> {
  return new Promise<XhrResult>((resolve, reject) => {
    if (opts.signal?.aborted) {
      reject(new TransferError('cancelled', 'The transfer was cancelled.'));
      return;
    }
    let xhr: XMLHttpRequest;
    try {
      xhr = new XMLHttpRequest();
      xhr.open(opts.method, opts.url, true);
      for (const [k, v] of Object.entries(opts.headers ?? {})) xhr.setRequestHeader(k, v);
    } catch (err) {
      reject(toTransferError(err, opts.host));
      return;
    }
    const onAbort = () => xhr.abort();
    const cleanup = () => opts.signal?.removeEventListener('abort', onAbort);
    opts.signal?.addEventListener('abort', onAbort, { once: true });

    if (opts.onUploadProgress && xhr.upload) {
      const cb = opts.onUploadProgress;
      xhr.upload.onprogress = (ev: ProgressEvent) => {
        if (ev.lengthComputable) cb(ev.loaded, ev.total);
      };
    }
    xhr.onload = () => {
      cleanup();
      resolve({
        status: xhr.status,
        responseText: typeof xhr.responseText === 'string' ? xhr.responseText : '',
        getResponseHeader: (n) => xhr.getResponseHeader(n),
      });
    };
    xhr.onerror = () => {
      cleanup();
      reject(networkError(opts.host));
    };
    xhr.onabort = () => {
      cleanup();
      reject(new TransferError('cancelled', 'The transfer was cancelled.'));
    };
    try {
      xhr.send(opts.body ?? null);
    } catch (err) {
      cleanup();
      reject(toTransferError(err, opts.host));
    }
  });
}

// ───────────── Send wrapper ─────────────

/**
 * Runs a send body. Emits 'failed' and rethrows a TransferError on any error.
 * The body returns remotePath. The wrapper adds timing and 'complete'.
 */
export async function runSend(
  file: OutputFile,
  onProgress: (p: TransferProgress) => void,
  host: string | undefined,
  body: () => Promise<string | undefined>,
): Promise<TransferResult> {
  const startedAt = new Date().toISOString();
  const emit = safeEmitter(onProgress);
  try {
    const remotePath = await body();
    emit({
      state: 'complete',
      fraction: 1,
      bytesSent: file.blob.size,
      bytesTotal: file.blob.size,
      message: remotePath ? `Saved as ${remotePath}` : 'Done',
    });
    return { ok: true, remotePath, bytes: file.blob.size, startedAt, finishedAt: new Date().toISOString() };
  } catch (err) {
    const te = toTransferError(err, host);
    emit({ state: 'failed', message: te.message });
    throw te;
  }
}

/** Wraps a progress callback so a bug in the UI callback cannot break a transfer. */
export function safeEmitter(cb: (p: TransferProgress) => void): (p: TransferProgress) => void {
  return (p) => {
    try {
      cb(p);
    } catch {
      /* Ignore errors in the UI callback. */
    }
  };
}

/** Reads a Blob into an ArrayBuffer. Uses FileReader when blob.arrayBuffer is missing. */
export function blobToArrayBuffer(blob: Blob): Promise<ArrayBuffer> {
  if (typeof blob.arrayBuffer === 'function') return blob.arrayBuffer();
  return new Promise((resolve, reject) => {
    const fr = new FileReader();
    fr.onload = () => resolve(fr.result as ArrayBuffer);
    fr.onerror = () => reject(fr.error ?? new Error('Could not read the file.'));
    fr.readAsArrayBuffer(blob);
  });
}

/** Waits ms milliseconds. Rejects with 'cancelled' when the signal aborts. */
export function delay(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    if (signal?.aborted) {
      reject(new TransferError('cancelled', 'The transfer was cancelled.'));
      return;
    }
    const t = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    const onAbort = () => {
      clearTimeout(t);
      reject(new TransferError('cancelled', 'The transfer was cancelled.'));
    };
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/** Shared limitation text for LAN adapters. */
export function lanLimitations(protocol: string = pageProtocol()): string[] {
  const out: string[] = [
    'The device must show the File Transfer screen and be on the same network as this computer.',
  ];
  if (protocol === 'https:') {
    out.push(
      'This page uses https and the device uses plain http. Most browsers block this as mixed content. ' +
        'Use a USB folder write, a manual download, the app on http://localhost, or the local helper app.',
    );
    out.push('Chrome Private Network Access can also block or ask before a public page talks to a LAN address.');
  }
  return out;
}
