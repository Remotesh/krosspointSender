/**
 * CrossPoint native HTTP upload (port 80, File Transfer screen).
 * Steps: POST /mkdir per folder segment (errors ignored), then
 * POST /upload?path=<dir> multipart field "file" with XHR progress.
 * When the device answers 400 "File already exists", POST /delete with
 * paths=["<path>"] and retry once. Verify with GET /api/files?path=<dir>.
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
import {
  baseUrl,
  fetchWithTimeout,
  folderNoTrailingSlash,
  folderSegments,
  httpError,
  lanLimitations,
  remoteFilePath,
  runSend,
  safeEmitter,
  toTransferError,
  xhrRequest,
  type XhrResult,
} from './common';

interface CrossPointFileEntry {
  name: string;
  size?: number;
  isDirectory?: boolean;
  isEpub?: boolean;
}

const EXISTS_RE = /already exists/i;

/**
 * Reads the JSON body of GET /api/status. A JSON value that is not an object
 * gives {}. A body that is not JSON throws a 'device' TransferError.
 * testConnection and the reader discovery (src/discovery) both use it.
 */
export async function readStatusInfo(res: { json(): Promise<unknown> }): Promise<Record<string, unknown>> {
  try {
    const data = await res.json();
    return data && typeof data === 'object' && !Array.isArray(data) ? (data as Record<string, unknown>) : {};
  } catch (err) {
    throw new TransferError('device', 'The device answered, but /api/status did not return JSON. Is this a CrossPoint device?', err);
  }
}

/**
 * True when /api/status info looks like CrossPoint: it has "version" and
 * "mode" ("AP" or "STA"). The desktop discovery uses the same test.
 */
export function isCrossPointStatus(info: Record<string, unknown>): boolean {
  return typeof info.version === 'string' && info.version !== '' && typeof info.mode === 'string';
}

export class CrossPointHttpTransport implements TransportAdapter {
  readonly method = 'crosspoint-http' as const;

  isSupported(): boolean {
    return typeof fetch === 'function' && typeof XMLHttpRequest !== 'undefined' && typeof FormData !== 'undefined';
  }

  limitations(): string[] {
    const out = lanLimitations();
    if (!this.isSupported()) out.unshift('This browser lacks fetch, XMLHttpRequest or FormData.');
    return out;
  }

  async testConnection(profile: DeviceProfile): Promise<ConnectionTestResult> {
    const t0 = Date.now();
    try {
      const res = await fetchWithTimeout(`${baseUrl(profile)}/api/status`, { method: 'GET', host: profile.host });
      if (!res.ok) throw httpError(res.status, 'The status request');
      const info = await readStatusInfo(res);
      return { ok: true, latencyMs: Date.now() - t0, info };
    } catch (err) {
      return { ok: false, latencyMs: Date.now() - t0, error: toTransferError(err, profile.host) };
    }
  }

  send(
    profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    const base = baseUrl(profile);
    const host = profile.host;
    const total = file.blob.size;
    const dir = folderNoTrailingSlash(profile.targetFolder);
    const remotePath = remoteFilePath(profile.targetFolder, file.name);

    return runSend(file, onProgress, host, async () => {
      // 1. Connecting: create each folder segment. CrossPoint needs the folder to exist.
      emit({ state: 'connecting', message: `Connecting to ${host}:${profile.port}` });
      let parent = '/';
      for (const seg of folderSegments(profile.targetFolder)) {
        const form = new URLSearchParams({ name: seg, path: parent });
        try {
          await fetchWithTimeout(`${base}/mkdir`, { method: 'POST', body: form, host }, signal);
        } catch (err) {
          // Ignore mkdir errors. The folder may exist. A real failure shows in the upload.
          if (err instanceof TransferError && err.code === 'cancelled') throw err;
        }
        parent = parent === '/' ? `/${seg}` : `${parent}/${seg}`;
      }

      // 2. Uploading: multipart POST. Retry once after delete when the file exists.
      const upload = (): Promise<XhrResult> => {
        const fd = new FormData();
        fd.append('file', file.blob, file.name);
        emit({ state: 'uploading', fraction: 0, bytesSent: 0, bytesTotal: total, message: `Uploading ${file.name}` });
        return xhrRequest({
          method: 'POST',
          url: `${base}/upload?path=${encodeURIComponent(dir)}`,
          body: fd,
          host,
          signal,
          onUploadProgress: (loaded, t) => {
            // The multipart body is a little larger than the file. Clamp to the file size.
            const frac = t > 0 ? Math.min(1, loaded / t) : undefined;
            emit({
              state: 'uploading',
              fraction: frac,
              bytesSent: frac !== undefined ? Math.round(frac * total) : undefined,
              bytesTotal: total,
            });
          },
        });
      };

      let res = await upload();
      if (res.status === 400 && EXISTS_RE.test(res.responseText)) {
        emit({ state: 'uploading', fraction: 0, message: 'The file exists on the device. Replacing it.' });
        const del = await fetchWithTimeout(
          `${base}/delete`,
          { method: 'POST', body: new URLSearchParams({ paths: JSON.stringify([remotePath]) }), host },
          signal,
        );
        if (!del.ok) {
          throw new TransferError(
            'exists',
            `${file.name} already exists on the device, and the device did not delete it (HTTP ${del.status}). Rename the file or delete it on the device.`,
          );
        }
        res = await upload();
        if (res.status === 400 && EXISTS_RE.test(res.responseText)) {
          throw new TransferError('exists', `${file.name} already exists on the device. Rename the file or delete it on the device.`);
        }
      }
      if (res.status < 200 || res.status >= 300) throw httpError(res.status, 'The upload', res.responseText);

      // 3. Verifying: list the folder and compare the size.
      emit({ state: 'verifying', fraction: 1, bytesSent: total, bytesTotal: total, message: 'Checking the file on the device' });
      const list = await fetchWithTimeout(`${base}/api/files?path=${encodeURIComponent(dir)}`, { method: 'GET', host }, signal);
      if (!list.ok) throw httpError(list.status, 'The check after upload');
      let entries: CrossPointFileEntry[];
      try {
        const data = await list.json();
        entries = Array.isArray(data) ? (data as CrossPointFileEntry[]) : [];
      } catch {
        throw new TransferError('device', 'The device returned a file list that is not JSON.');
      }
      const found = entries.find((e) => e && e.name === file.name && !e.isDirectory);
      if (!found) {
        throw new TransferError('device', `The upload finished, but ${file.name} is not in ${dir} on the device.`);
      }
      if (typeof found.size === 'number' && found.size !== total) {
        throw new TransferError(
          'device',
          `The device reports ${found.size} bytes, but the file has ${total} bytes. Send the file again.`,
        );
      }
      return remotePath;
    });
  }
}

export const crosspointHttpTransport = new CrossPointHttpTransport();
