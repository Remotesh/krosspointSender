/**
 * Manual download. Always supported. The browser saves the file.
 * The user then copies it to the reader (USB Drive mode or the CrossPoint web page).
 */
import type {
  ConnectionTestResult,
  DeviceProfile,
  OutputFile,
  TransferProgress,
  TransferResult,
  TransportAdapter,
} from '../types';
import { TransferError } from '../types';
import { runSend, safeEmitter } from './common';

/** Starts a browser download of a blob. Revokes the object URL later. */
export function downloadBlob(blob: Blob, name: string): void {
  if (typeof URL === 'undefined' || typeof URL.createObjectURL !== 'function' || typeof document === 'undefined') {
    throw new TransferError('unsupported', 'This browser cannot start a download.');
  }
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = name;
  a.rel = 'noopener';
  a.style.display = 'none';
  document.body.appendChild(a);
  try {
    a.click();
  } finally {
    a.remove();
    // Give the browser time to start the download before the URL goes away.
    setTimeout(() => {
      try {
        URL.revokeObjectURL(url);
      } catch {
        /* ignore */
      }
    }, 30_000);
  }
}

export class DownloadTransport implements TransportAdapter {
  readonly method = 'download' as const;

  isSupported(): boolean {
    return true;
  }

  limitations(): string[] {
    return ['The file goes to your Downloads folder. Copy it to the reader yourself.'];
  }

  async testConnection(_profile: DeviceProfile): Promise<ConnectionTestResult> {
    return { ok: true, latencyMs: 0, info: { method: 'download' } };
  }

  send(
    _profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    return runSend(file, onProgress, undefined, async () => {
      if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
      emit({ state: 'uploading', fraction: 0, bytesTotal: file.blob.size, message: `Downloading ${file.name}` });
      downloadBlob(file.blob, file.name);
      return file.name;
    });
  }
}

export const downloadTransport = new DownloadTransport();
