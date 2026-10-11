/**
 * Local helper transport. The optional helper app (helper/helper.py) runs on
 * this computer at settings.helperUrl (default http://127.0.0.1:8134).
 * Browsers treat 127.0.0.1 as a secure origin, so an https page can call it.
 * The helper then sends the file to the device over plain http on the LAN.
 *
 * API:
 *   GET  /status -> { ok, version }
 *   POST /send   multipart: file, host, port, folder, method (+ username, password)
 *                -> { ok, remotePath, bytes } | { ok: false, error, code? }
 *   POST /test   JSON { host, port } -> { ok, info, latencyMs } | { ok: false, error }
 *
 * TransferMethod has no "helper" value. This adapter reports method "webdav"
 * and forwards profile.method: "crosspoint-http" stays, everything else uses WebDAV.
 * It is not in listTransports(). Get it with getHelperTransport().
 */
import {
  TransferError,
  type ConnectionTestResult,
  type DeviceProfile,
  type OutputFile,
  type TransferErrorCode,
  type TransferMethod,
  type TransferProgress,
  type TransferResult,
  type TransportAdapter,
} from '../types';
import { DEFAULT_HELPER_URL, loadSettings } from '../settings';
import {
  fetchWithTimeout,
  httpError,
  normalizeFolder,
  remoteFilePath,
  runSend,
  safeEmitter,
  toTransferError,
  xhrRequest,
} from './common';

const KNOWN_CODES: TransferErrorCode[] = [
  'network',
  'mixed-content',
  'not-found',
  'auth',
  'exists',
  'unsupported',
  'cancelled',
  'device',
  'unknown',
];

function helperHost(url: string): string {
  try {
    return new URL(url).hostname;
  } catch {
    return '127.0.0.1';
  }
}

function notRunning(url: string, cause?: unknown): TransferError {
  return new TransferError(
    'network',
    `The local helper app does not answer at ${url}. Start it with "python helper.py" and try again.`,
    cause,
  );
}

/** The method the helper uses to talk to the device. */
export function helperForwardMethod(method: TransferMethod): 'webdav' | 'crosspoint-http' {
  return method === 'crosspoint-http' ? 'crosspoint-http' : 'webdav';
}

export class HelperTransport implements TransportAdapter {
  readonly method: TransferMethod = 'webdav';
  /** Marks this adapter as the helper path. */
  readonly viaHelper = true;

  constructor(private urlOverride?: string) {}

  /** Sets a fixed helper URL. Pass undefined to use the settings again. */
  setHelperUrl(url: string | undefined): void {
    this.urlOverride = url;
  }

  async helperUrl(): Promise<string> {
    let url = this.urlOverride;
    if (!url) {
      try {
        url = (await loadSettings()).helperUrl;
      } catch {
        url = undefined;
      }
    }
    return (url || DEFAULT_HELPER_URL).replace(/\/+$/, '');
  }

  isSupported(): boolean {
    return typeof fetch === 'function' && typeof XMLHttpRequest !== 'undefined' && typeof FormData !== 'undefined';
  }

  limitations(): string[] {
    return [
      'The helper app must run on this computer (python helper.py). It listens on 127.0.0.1:8134.',
      'The helper sends the file to the device on your LAN. Nothing goes to the internet.',
    ];
  }

  /** GET /status on the helper. */
  async status(signal?: AbortSignal): Promise<{ ok: boolean; version?: string }> {
    const url = await this.helperUrl();
    try {
      const res = await fetchWithTimeout(`${url}/status`, { method: 'GET', host: helperHost(url) }, signal);
      if (!res.ok) throw httpError(res.status, 'The helper status request');
      return (await res.json()) as { ok: boolean; version?: string };
    } catch (err) {
      const te = toTransferError(err, helperHost(url));
      if (te.code === 'network' || te.code === 'not-found') throw notRunning(url, te);
      throw te;
    }
  }

  async testConnection(profile: DeviceProfile): Promise<ConnectionTestResult> {
    const t0 = Date.now();
    const url = await this.helperUrl();
    let helperInfo: { ok: boolean; version?: string };
    try {
      helperInfo = await this.status();
    } catch (err) {
      return { ok: false, latencyMs: Date.now() - t0, error: toTransferError(err) };
    }
    try {
      const res = await fetchWithTimeout(`${url}/test`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ host: profile.host, port: profile.port }),
        host: helperHost(url),
        timeoutMs: 10_000,
      });
      const data = (await res.json().catch(() => ({}))) as { ok?: boolean; info?: Record<string, unknown>; error?: string };
      if (!res.ok || !data.ok) {
        return {
          ok: false,
          latencyMs: Date.now() - t0,
          info: { helper: helperInfo },
          error: new TransferError(
            'not-found',
            `The helper runs, but the device did not answer: ${data.error ?? `HTTP ${res.status}`}.`,
          ),
        };
      }
      return { ok: true, latencyMs: Date.now() - t0, info: { helper: helperInfo, device: data.info ?? {} } };
    } catch (err) {
      return { ok: false, latencyMs: Date.now() - t0, error: toTransferError(err) };
    }
  }

  send(
    profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    const total = file.blob.size;

    return runSend(file, onProgress, undefined, async () => {
      const url = await this.helperUrl();
      emit({ state: 'connecting', message: `Connecting to the helper at ${url}` });

      const fd = new FormData();
      fd.append('file', file.blob, file.name);
      fd.append('host', profile.host);
      fd.append('port', String(profile.port));
      fd.append('folder', normalizeFolder(profile.targetFolder));
      fd.append('method', helperForwardMethod(profile.method));
      if (profile.username) {
        fd.append('username', profile.username);
        fd.append('password', profile.password ?? '');
      }

      emit({ state: 'uploading', fraction: 0, bytesSent: 0, bytesTotal: total, message: `Sending ${file.name} to the helper` });
      let verifying = false;
      let res;
      try {
        res = await xhrRequest({
          method: 'POST',
          url: `${url}/send`,
          body: fd,
          host: helperHost(url),
          signal,
          onUploadProgress: (loaded, t) => {
            const frac = t > 0 ? Math.min(1, loaded / t) : undefined;
            emit({ state: 'uploading', fraction: frac, bytesSent: frac !== undefined ? Math.round(frac * total) : undefined, bytesTotal: total });
            if (frac === 1 && !verifying) {
              verifying = true;
              emit({ state: 'verifying', fraction: 1, message: 'The helper sends the file to the device' });
            }
          },
        });
      } catch (err) {
        const te = toTransferError(err);
        if (te.code === 'network') throw notRunning(url, te);
        throw te;
      }
      if (!verifying) emit({ state: 'verifying', fraction: 1, message: 'The helper sends the file to the device' });

      let data: { ok?: boolean; remotePath?: string; bytes?: number; error?: string; code?: string } = {};
      try {
        data = JSON.parse(res.responseText || '{}');
      } catch {
        if (res.status >= 400) throw httpError(res.status, 'The helper', res.responseText);
        throw new TransferError('unknown', 'The helper returned an answer that is not JSON.');
      }
      if (!data.ok) {
        const code = KNOWN_CODES.includes(data.code as TransferErrorCode) ? (data.code as TransferErrorCode) : 'device';
        throw new TransferError(code, `The helper could not send the file: ${data.error ?? `HTTP ${res.status}`}`);
      }
      if (typeof data.bytes === 'number' && data.bytes !== total) {
        throw new TransferError('device', `The helper sent ${data.bytes} bytes, but the file has ${total} bytes.`);
      }
      return data.remotePath ?? remoteFilePath(profile.targetFolder, file.name);
    });
  }
}

let helperSingleton: HelperTransport | null = null;

/** The helper adapter singleton. */
export function getHelperTransport(): HelperTransport {
  if (!helperSingleton) helperSingleton = new HelperTransport();
  return helperSingleton;
}
