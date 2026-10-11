/**
 * CrossPoint WebSocket upload on port 81.
 * Protocol:
 *   client -> text   START:<filename>:<size>:<path>
 *   device -> text   READY
 *   client -> binary frames (16 KiB chunks)
 *   device -> text   PROGRESS:<received>:<total>   (many)
 *   device -> text   DONE | ERROR:<message>
 * Flow control: send the next chunk only when bufferedAmount < 64 KiB.
 * Poll every 10 ms. This keeps the browser from queueing the whole file.
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
  CONNECT_TIMEOUT_MS,
  blobToArrayBuffer,
  folderNoTrailingSlash,
  lanLimitations,
  networkError,
  remoteFilePath,
  runSend,
  safeEmitter,
  toTransferError,
} from './common';

export const WS_PORT = 81;
export const WS_CHUNK_SIZE = 16 * 1024;
export const WS_HIGH_WATER = 64 * 1024;
const POLL_MS = 10;

export function wsUrl(host: string): string {
  return `ws://${host}:${WS_PORT}/`;
}

export class CrossPointWsTransport implements TransportAdapter {
  readonly method = 'crosspoint-ws' as const;

  isSupported(): boolean {
    return typeof WebSocket !== 'undefined';
  }

  limitations(): string[] {
    const out = lanLimitations();
    out.push(`The WebSocket upload uses port ${WS_PORT}. The port in the profile is not used.`);
    if (!this.isSupported()) out.unshift('This browser has no WebSocket support.');
    return out;
  }

  testConnection(profile: DeviceProfile): Promise<ConnectionTestResult> {
    const t0 = Date.now();
    return new Promise<ConnectionTestResult>((resolve) => {
      let ws: WebSocket;
      let settled = false;
      const finish = (r: ConnectionTestResult) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        try {
          ws?.close();
        } catch {
          /* ignore */
        }
        resolve({ latencyMs: Date.now() - t0, ...r });
      };
      const timer = setTimeout(
        () =>
          finish({
            ok: false,
            error: new TransferError('not-found', `${profile.host}:${WS_PORT} did not answer within ${CONNECT_TIMEOUT_MS / 1000} s.`),
          }),
        CONNECT_TIMEOUT_MS,
      );
      try {
        ws = new WebSocket(wsUrl(profile.host));
      } catch (err) {
        finish({ ok: false, error: toTransferError(err, profile.host) });
        return;
      }
      ws.onopen = () => finish({ ok: true, info: { url: wsUrl(profile.host), port: WS_PORT } });
      ws.onerror = () => finish({ ok: false, error: networkError(profile.host) });
      ws.onclose = () => finish({ ok: false, error: networkError(profile.host) });
    });
  }

  send(
    profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    const host = profile.host;
    const total = file.blob.size;
    const dir = folderNoTrailingSlash(profile.targetFolder);
    const remotePath = remoteFilePath(profile.targetFolder, file.name);

    return runSend(file, onProgress, host, async () => {
      if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
      if (/[:\r\n]/.test(file.name)) {
        throw new TransferError('device', 'The file name contains ":" or a line break. The WebSocket protocol cannot send it.');
      }
      const data = await blobToArrayBuffer(file.blob);
      emit({ state: 'connecting', message: `Connecting to ${host}:${WS_PORT}` });

      await new Promise<void>((resolve, reject) => {
        let ws: WebSocket;
        let phase: 'connecting' | 'waiting-ready' | 'sending' | 'waiting-done' | 'finished' = 'connecting';
        let connectTimer: ReturnType<typeof setTimeout> | undefined;
        let pollTimer: ReturnType<typeof setTimeout> | undefined;

        const end = (err?: TransferError) => {
          if (phase === 'finished') return;
          phase = 'finished';
          clearTimeout(connectTimer);
          clearTimeout(pollTimer);
          signal?.removeEventListener('abort', onAbort);
          try {
            ws?.close();
          } catch {
            /* ignore */
          }
          if (err) reject(err);
          else resolve();
        };
        const onAbort = () => end(new TransferError('cancelled', 'The transfer was cancelled.'));
        signal?.addEventListener('abort', onAbort, { once: true });

        try {
          ws = new WebSocket(wsUrl(host));
          ws.binaryType = 'arraybuffer';
        } catch (err) {
          end(toTransferError(err, host));
          return;
        }

        connectTimer = setTimeout(
          () =>
            end(
              new TransferError(
                'not-found',
                `${host}:${WS_PORT} did not answer within ${CONNECT_TIMEOUT_MS / 1000} s. Check the address and that the device shows the File Transfer screen.`,
              ),
            ),
          CONNECT_TIMEOUT_MS,
        );

        let offset = 0;
        const pump = () => {
          if (phase !== 'sending') return;
          try {
            while (offset < data.byteLength && ws.bufferedAmount < WS_HIGH_WATER) {
              const next = Math.min(offset + WS_CHUNK_SIZE, data.byteLength);
              ws.send(data.slice(offset, next));
              offset = next;
            }
          } catch (err) {
            end(toTransferError(err, host));
            return;
          }
          if (offset >= data.byteLength) {
            phase = 'waiting-done';
            return;
          }
          pollTimer = setTimeout(pump, POLL_MS);
        };

        ws.onopen = () => {
          if (phase !== 'connecting') return;
          phase = 'waiting-ready';
          try {
            ws.send(`START:${file.name}:${total}:${dir}`);
          } catch (err) {
            end(toTransferError(err, host));
          }
        };

        ws.onmessage = (ev: MessageEvent) => {
          if (typeof ev.data !== 'string') return;
          const msg = ev.data.trim();
          if (msg === 'READY') {
            if (phase !== 'waiting-ready') return;
            clearTimeout(connectTimer);
            phase = 'sending';
            emit({ state: 'uploading', fraction: 0, bytesSent: 0, bytesTotal: total, message: `Uploading ${file.name}` });
            if (total === 0) phase = 'waiting-done';
            else pump();
          } else if (msg.startsWith('PROGRESS:')) {
            const [, recv, tot] = msg.split(':');
            const r = Number(recv);
            const t = Number(tot) || total;
            if (Number.isFinite(r)) {
              emit({ state: 'uploading', fraction: t > 0 ? Math.min(1, r / t) : undefined, bytesSent: r, bytesTotal: t });
              if (r >= t) {
                emit({ state: 'verifying', fraction: 1, bytesSent: r, bytesTotal: t, message: 'Waiting for the device to confirm' });
              }
            }
          } else if (msg === 'DONE') {
            end();
          } else if (msg.startsWith('ERROR')) {
            const text = msg.slice(msg.indexOf(':') + 1).trim() || 'unknown error';
            end(new TransferError(text.toLowerCase().includes('exist') ? 'exists' : 'device', `The device reported an error: ${text}`));
          }
        };

        ws.onerror = () => end(networkError(host));
        ws.onclose = (ev: CloseEvent) => {
          if (phase === 'finished') return;
          if (phase === 'connecting') end(networkError(host));
          else
            end(
              new TransferError(
                'device',
                `The device closed the connection before it confirmed the upload${ev?.code ? ` (code ${ev.code})` : ''}.`,
              ),
            );
        };
      });
      return remotePath;
    });
  }
}

export const crosspointWsTransport = new CrossPointWsTransport();
