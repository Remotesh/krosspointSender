/**
 * BLE transport (MOCK). No radio is used. send() simulates the pipeline in
 * about 1.2 s and emits real progress events, so the UI can be built and
 * tested now. The real adapter will replace the simulated steps with Web
 * Bluetooth calls when the K-Reader firmware ships the service below.
 *
 * ───────────────── Future K-Reader BLE file transfer protocol (draft) ─────────────────
 *
 * Transport: BLE GATT. The reader is the peripheral. The browser is the central
 * (navigator.bluetooth.requestDevice with a filter on the service UUID).
 * All integers are little-endian. All strings are UTF-8.
 *
 * Service
 *   KREADER_FILE_SERVICE   6b726472-0001-4b52-8000-00000000f11e   (placeholder, not final)
 *
 * Characteristics
 *   METADATA  ...-0002  write (with response)
 *       Payload: u8 version (=1) | u32 fileSize | u32 crc32 | u16 chunkSize |
 *                u8 nameLen | name[nameLen] | u8 pathLen | path[pathLen]
 *       path is the target folder. Default "/Books/K-rosspoint/".
 *       The reader answers on STATUS with READY or ERROR.
 *   CHUNK     ...-0003  write without response
 *       Payload: u32 offset | data[<= 244 - 4]
 *       244 bytes is the payload of a 247-byte ATT MTU. The client asks for
 *       MTU 247. With a smaller MTU, chunk = MTU - 3 - 4 bytes of data.
 *       The client sends chunks in order. It pauses every 32 chunks until
 *       STATUS reports ACK for that offset (simple windowed flow control).
 *   CONTROL   ...-0004  write (with response)
 *       Opcodes: 0x01 BEGIN (after METADATA), 0x02 VERIFY, 0x03 COMMIT,
 *                0x04 ABORT, 0x05 PING.
 *   STATUS    ...-0005  notify
 *       Payload: u8 code | u32 value | optional UTF-8 text
 *       Codes: 0x00 READY, 0x01 ACK(offset received), 0x02 CRC_OK(crc32),
 *              0x03 CRC_BAD(crc32 seen), 0x04 COMMITTED, 0x05 ERROR(text),
 *              0x06 BUSY, 0x07 NO_SPACE.
 *
 * Sequence
 *   1. Pair / connect. Subscribe to STATUS.
 *   2. Write METADATA. Wait for READY.
 *   3. CONTROL BEGIN. Stream CHUNK writes. Progress = last ACK offset / fileSize.
 *   4. CONTROL VERIFY. The reader computes CRC32 (IEEE 802.3, poly 0xEDB88320)
 *      over the received bytes. CRC_OK continues. CRC_BAD restarts from step 2.
 *   5. CONTROL COMMIT. The reader moves the temp file to <path><name>
 *      (default /Books/K-rosspoint/<name>) and replies COMMITTED.
 *   6. Disconnect. ABORT at any time deletes the temp file.
 *
 * Security: the reader accepts writes only while its "Receive via Bluetooth"
 * screen is open. The link is local. No server is involved.
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

export const BLE_SERVICE_UUID = '6b726472-0001-4b52-8000-00000000f11e';
export const BLE_CHUNK_SIZE = 244;
export const BLE_DEFAULT_FOLDER = '/Books/K-rosspoint/';

/** Simulated step times in ms. Total about 1.2 s. */
const T_PAIR = 200;
const T_META = 100;
const T_CHUNKS = 600;
const T_CHECKSUM = 200;
const T_COMMIT = 100;
const PROGRESS_STEPS = 12;

let crcTable: Uint32Array | null = null;

/** CRC32 (IEEE 802.3), as the future protocol uses for verification. */
export function crc32(data: Uint8Array): number {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let crc = 0xffffffff;
  for (let i = 0; i < data.length; i++) crc = crcTable[(crc ^ data[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ 0xffffffff) >>> 0;
}

export class BleMockTransport implements TransportAdapter {
  readonly method = 'ble' as const;

  isSupported(): boolean {
    return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
  }

  limitations(): string[] {
    const out = [
      'Bluetooth transfer is a simulation. No data leaves this computer.',
      'It needs the future K-Reader firmware. CrossPoint has no Bluetooth file transfer.',
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
