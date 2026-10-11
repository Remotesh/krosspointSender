/**
 * WebDAV transport. CrossPoint serves WebDAV at the root on port 80.
 * Steps: OPTIONS (connecting), MKCOL per folder segment, PUT with XHR
 * upload progress (uploading), HEAD to check Content-Length (verifying).
 * PUT overwrites. The device returns 201 for a new file, 204 for a replaced file.
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
  basicAuthHeader,
  baseUrl,
  encodePath,
  fetchWithTimeout,
  folderSegments,
  httpError,
  lanLimitations,
  remoteFilePath,
  runSend,
  safeEmitter,
  toTransferError,
  xhrRequest,
} from './common';

function authHeaders(profile: DeviceProfile): Record<string, string> {
  const auth = basicAuthHeader(profile);
  return auth ? { Authorization: auth } : {};
}

export class WebDavTransport implements TransportAdapter {
  readonly method = 'webdav' as const;

  isSupported(): boolean {
    return typeof fetch === 'function' && typeof XMLHttpRequest !== 'undefined';
  }

  limitations(): string[] {
    const out = lanLimitations();
    if (!this.isSupported()) out.unshift('This browser lacks fetch or XMLHttpRequest.');
    return out;
  }

  async testConnection(profile: DeviceProfile): Promise<ConnectionTestResult> {
    const t0 = Date.now();
    try {
      const res = await fetchWithTimeout(`${baseUrl(profile)}/`, {
        method: 'OPTIONS',
        headers: authHeaders(profile),
        host: profile.host,
      });
      if (!res.ok) throw httpError(res.status, 'The WebDAV check');
      return {
        ok: true,
        latencyMs: Date.now() - t0,
        info: {
          status: res.status,
          dav: res.headers.get('DAV'),
          allow: res.headers.get('Allow'),
        },
      };
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
    const headers = authHeaders(profile);
    const host = profile.host;
    const total = file.blob.size;

    return runSend(file, onProgress, host, async () => {
      // 1. Connecting: OPTIONS.
      emit({ state: 'connecting', message: `Connecting to ${host}:${profile.port}` });
      const opt = await fetchWithTimeout(`${base}/`, { method: 'OPTIONS', headers, host }, signal);
      if (opt.status === 401 || opt.status === 403) throw httpError(opt.status, 'The connection');

      // 2. MKCOL each folder segment. 405/409 means the folder exists.
      let path = '/';
      for (const seg of folderSegments(profile.targetFolder)) {
        path += `${seg}/`;
        const res = await fetchWithTimeout(`${base}${encodePath(path)}`, { method: 'MKCOL', headers, host }, signal);
        if (res.ok || res.status === 405 || res.status === 409 || res.status === 301) continue;
        throw httpError(res.status, `Creating the folder ${path}`, await safeText(res));
      }

      // 3. Uploading: PUT with real upload progress.
      const remotePath = remoteFilePath(profile.targetFolder, file.name);
      const url = `${base}${encodePath(remotePath)}`;
      emit({ state: 'uploading', fraction: 0, bytesSent: 0, bytesTotal: total, message: `Uploading ${file.name}` });
      const put = await xhrRequest({
        method: 'PUT',
        url,
        body: file.blob,
        headers: { ...headers, 'Content-Type': file.mime || 'application/octet-stream' },
        host,
        signal,
        onUploadProgress: (loaded, t) =>
          emit({
            state: 'uploading',
            fraction: t > 0 ? loaded / t : undefined,
            bytesSent: loaded,
            bytesTotal: t || total,
          }),
      });
      if (put.status < 200 || put.status >= 300) throw httpError(put.status, 'The upload', put.responseText);

      // 4. Verifying: HEAD. Compare Content-Length when the server returns it.
      emit({ state: 'verifying', fraction: 1, bytesSent: total, bytesTotal: total, message: 'Checking the file on the device' });
      const head = await fetchWithTimeout(url, { method: 'HEAD', headers, host }, signal);
      if (!head.ok) throw httpError(head.status, 'The check after upload');
      const len = head.headers.get('Content-Length');
      if (len !== null && len !== '' && Number(len) !== total) {
        throw new TransferError(
          'device',
          `The device reports ${len} bytes, but the file has ${total} bytes. Send the file again.`,
        );
      }
      return remotePath;
    });
  }
}

async function safeText(res: { text(): Promise<string> }): Promise<string> {
  try {
    return await res.text();
  } catch {
    return '';
  }
}

export const webdavTransport = new WebDavTransport();
