/**
 * BLE transport (MOCK), for tests and development only. No radio is used.
 * send() simulates the pipeline in about 1.2 s and emits progress events.
 *
 * The real transport is ble.ts (CrossPoint BLE file transfer protocol over
 * Web Bluetooth, contract in docs/BLE_PROTOCOL.md). The registry uses this
 * mock for method 'ble' only in a dev build (npm run dev) with the flag
 * ?blemock in the URL or localStorage "krosspoint.bleMock" = "1".
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
import { blobToArrayBuffer, delay, normalizeFolder, remoteFilePath, runSend, safeEmitter } from './common';
import { crc32 } from './crc32';
import { BLE_DEFAULT_FOLDER, BLE_SERVICE_UUID } from './ble';

/** Chunk size the mock pretends to use (ATT MTU 247 - 7). */
export const BLE_CHUNK_SIZE = 240;

/** Simulated step times in ms. Total about 1.2 s. */
const T_PAIR = 200;
const T_META = 100;
const T_CHUNKS = 600;
const T_CHECKSUM = 200;
const T_COMMIT = 100;
const PROGRESS_STEPS = 12;

export { crc32 };

export class BleMockTransport implements TransportAdapter {
  readonly method = 'ble' as const;

  isSupported(): boolean {
    return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
  }

  limitations(): string[] {
    const out = [
      'Bluetooth transfer is a simulation (development mock). No data leaves this computer.',
    ];
    if (!this.isSupported()) out.push('This browser has no Web Bluetooth. Chrome and Edge on desktop and Android have it.');
    return out;
  }

  async testConnection(_profile: DeviceProfile): Promise<ConnectionTestResult> {
    await delay(T_PAIR);
    return {
      ok: true,
      latencyMs: T_PAIR,
      info: { simulated: true, service: BLE_SERVICE_UUID, chunkSize: BLE_CHUNK_SIZE, webBluetooth: this.isSupported() },
    };
  }

  send(
    profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult> {
    const emit = safeEmitter(onProgress);
    const total = file.blob.size;
    const folder = profile.targetFolder ? normalizeFolder(profile.targetFolder) : BLE_DEFAULT_FOLDER;

    return runSend(file, onProgress, undefined, async () => {
      if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
      const bytes = new Uint8Array(await blobToArrayBuffer(file.blob));
      const chunks = Math.max(1, Math.ceil(total / BLE_CHUNK_SIZE));

      emit({ state: 'connecting', message: 'Pairing with the reader (simulated)' });
      await delay(T_PAIR, signal);
      emit({ state: 'connecting', message: `Sending metadata: ${file.name}, ${total} bytes (simulated)` });
      await delay(T_META, signal);

      for (let i = 1; i <= PROGRESS_STEPS; i++) {
        await delay(T_CHUNKS / PROGRESS_STEPS, signal);
        const sent = Math.round((total * i) / PROGRESS_STEPS);
        emit({
          state: 'uploading',
          fraction: i / PROGRESS_STEPS,
          bytesSent: sent,
          bytesTotal: total,
          message: `Chunk ${Math.ceil((chunks * i) / PROGRESS_STEPS)} of ${chunks} (simulated)`,
        });
      }

      const crc = crc32(bytes);
      emit({
        state: 'verifying',
        fraction: 1,
        bytesSent: total,
        bytesTotal: total,
        message: `Checking CRC32 ${crc.toString(16).padStart(8, '0')} (simulated)`,
      });
      await delay(T_CHECKSUM, signal);
      emit({ state: 'verifying', fraction: 1, message: `Committing to ${folder} (simulated)` });
      await delay(T_COMMIT, signal);
      return remoteFilePath(folder, file.name);
    });
  }
}

export const bleMockTransport = new BleMockTransport();
