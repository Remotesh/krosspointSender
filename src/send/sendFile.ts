/**
 * "Send a file": put any file from disk on the reader's SD card with the
 * active transport. Nothing is converted. Typical use: upload a CrossPoint
 * firmware image (firmware.bin) to "/", then install it on the reader from
 * Settings > System > SD Card Firmware Update.
 *
 * The send goes through the same TransportAdapter.send() as an article send:
 * the same folder normalisation, overwrite rules, verification and cancel.
 * The send is logged in the transfer log (kind 'file'). No library entry is made.
 */
import type {
  DeviceProfile,
  OutputFile,
  TransferProgress,
  TransferRecord,
  TransferResult,
  TransportAdapter,
} from '../types';
import { TransferError } from '../types';
import { normalizeFolder, remoteFilePath } from '../transports/common';
import { BLE_MAX_BYTES, BLE_WARN_BYTES } from '../transports/ble';

/** Largest file "Send a file" accepts, for any transport. */
export const MAX_SEND_FILE_BYTES = 256 * 1024 * 1024;

/** Hint after a firmware (.bin) file is on the card. The menu path is the CrossPoint one. */
export const FIRMWARE_HINT = 'On the reader: Settings > System > SD Card Firmware Update, then pick this file.';

const MIME_BY_EXT: Record<string, string> = {
  bin: 'application/octet-stream',
  epub: 'application/epub+zip',
  txt: 'text/plain',
  xtc: 'application/octet-stream',
  xtch: 'application/octet-stream',
  bmp: 'image/bmp',
  png: 'image/png',
  jpg: 'image/jpeg',
  jpeg: 'image/jpeg',
  pdf: 'application/pdf',
  zip: 'application/zip',
  html: 'text/html',
  htm: 'text/html',
  md: 'text/markdown',
  json: 'application/json',
  ttf: 'font/ttf',
  otf: 'font/otf',
};

/** File extensions the article converter reads. Other files go to "Send a file". */
const ARTICLE_EXTS = new Set(['html', 'htm', 'xhtml', 'md', 'markdown', 'txt', 'text']);

/** Lower-case extension without the dot. Empty when there is none. */
export function fileExt(name: string): string {
  const base = name.split(/[\\/]/).pop() ?? '';
  const i = base.lastIndexOf('.');
  return i > 0 ? base.slice(i + 1).toLowerCase() : '';
}

/** True for a firmware image (.bin). */
export function isFirmwareFile(name: string): boolean {
  return fileExt(name) === 'bin';
}

/**
 * True when the article converter can read this file (HTML, Markdown, TXT,
 * or a text file with no extension). Other files are offered to "Send a file".
 */
export function isArticleFile(file: { name: string; type?: string }): boolean {
  const ext = fileExt(file.name);
  if (ARTICLE_EXTS.has(ext)) return true;
  const type = (file.type ?? '').toLowerCase();
  if (!ext) return type === '' || type.startsWith('text/');
  return type === 'text/html' || type === 'text/markdown' || type === 'text/plain';
}

/** The mime type for a file: the extension wins, then the browser type, then octet-stream. */
export function mimeFor(name: string, browserType?: string): string {
  return MIME_BY_EXT[fileExt(name)] ?? (browserType || 'application/octet-stream');
}

/**
 * A clean file name for the device: the last path segment, no control
 * characters, trimmed. Throws a TransferError when nothing is left.
 */
export function cleanFileName(name: string): string {
  // eslint-disable-next-line no-control-regex
  const base = (name.split(/[\\/]/).pop() ?? '').replace(/[\x00-\x1f\x7f]/g, '').trim();
  if (!base || base === '.' || base === '..') {
    throw new TransferError('device', 'Type a file name.');
  }
  return base;
}

/** Wraps a File (or Blob) as an OutputFile with format 'file'. The name can be changed. */
export function toOutputFile(file: Blob & { name?: string }, name?: string): OutputFile {
  const finalName = cleanFileName(name ?? file.name ?? '');
  return { name: finalName, mime: mimeFor(finalName, file.type), blob: file, format: 'file' };
}

/** A copy of the profile that sends to another folder. The folder is normalised ("/a//b" -> "/a/b/"). */
export function profileWithFolder(profile: DeviceProfile, folder: string | undefined): DeviceProfile {
  if (folder === undefined || folder.trim() === '') return profile;
  return { ...profile, targetFolder: normalizeFolder(folder) };
}

function mb(bytes: number): string {
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export interface SizeCheck {
  /** Set when the file must not be sent. */
  error?: TransferError;
  /** A warning to show. The send can go on. */
  warning?: string;
}

/**
 * Size rules before any device work. Any transport: refuse empty files and
 * files over 256 MB. Bluetooth: warn over 3 MB, refuse over 16 MB, and say
 * that Wi-Fi is better for firmware.
 */
export function checkFileSize(name: string, bytes: number, method: DeviceProfile['method']): SizeCheck {
  if (bytes <= 0) return { error: new TransferError('device', `${name} is empty. Nothing to send.`) };
  if (bytes > MAX_SEND_FILE_BYTES) {
    return {
      error: new TransferError(
        'too-large',
        `${name} is ${mb(bytes)}. "Send a file" takes files up to 256 MB. Copy larger files with a card reader or USB Drive mode.`,
      ),
    };
  }
  if (method !== 'ble') return {};
  const firmware = isFirmwareFile(name);
  if (bytes > BLE_MAX_BYTES) {
    return {
      error: new TransferError(
        'too-large',
        `${name} is ${mb(bytes)}. Bluetooth transfer is limited to 16 MB. Send it over Wi-Fi or USB.` +
          (firmware ? ' For firmware, Wi-Fi is better.' : ''),
      ),
    };
  }
  if (bytes > BLE_WARN_BYTES) {
    const minutes = Math.ceil(bytes / (20 * 1024) / 60);
    return {
      warning:
        `${name} is ${mb(bytes)}. Bluetooth sends about 10 to 50 KB/s, so this can take ${minutes} minutes or more.` +
        (firmware ? ' For firmware, Wi-Fi is better: it is faster and more reliable.' : ' Wi-Fi or USB is faster.'),
    };
  }
  if (firmware) {
    return { warning: 'Bluetooth can send firmware, but Wi-Fi is better: it is faster and more reliable.' };
  }
  return {};
}

export interface SendRawFileOptions {
  profile: DeviceProfile;
  file: Blob & { name?: string };
  /** Name on the device. Default: the file's own name. */
  name?: string;
  /** Destination folder. Default: the profile's targetFolder. */
  folder?: string;
  /** The transport for profile.method (the caller gets it from the registry). */
  transport: TransportAdapter;
  onProgress?: (p: TransferProgress) => void;
  signal?: AbortSignal;
  /** Writes the transfer record. A log error never fails the send. */
  log?: (rec: TransferRecord) => Promise<void>;
  /** For tests. */
  now?: () => Date;
  newId?: () => string;
}

export interface SendRawFileOutcome {
  result: TransferResult;
  file: OutputFile;
  /** The folder used, normalised. */
  folder: string;
  /** Remote path from the transport, or folder + name. */
  remotePath: string;
  /** Size warning shown before the send (Bluetooth). */
  warning?: string;
  /** Set for a .bin file: what to do on the reader. */
  hint?: string;
  record: TransferRecord;
}

/**
 * Send one file from disk. Throws a TransferError on a refusal or a failed
 * send. A send that reached the transport is logged, ok or not. A refusal
 * before the transport (size, name) is not logged.
 */
export async function sendRawFile(opts: SendRawFileOptions): Promise<SendRawFileOutcome> {
  const { transport, signal } = opts;
  const report = (p: TransferProgress): void => {
    try {
      opts.onProgress?.(p);
    } catch {
      /* A UI error does not stop a send. */
    }
  };
  let out: OutputFile;
  try {
    out = toOutputFile(opts.file, opts.name);
  } catch (err) {
    report({ state: 'failed', message: (err as Error).message });
    throw err;
  }
  const profile = profileWithFolder(opts.profile, opts.folder);
  const folder = normalizeFolder(profile.targetFolder);

  const size = checkFileSize(out.name, out.blob.size, profile.method);
  if (size.error) {
    report({ state: 'failed', message: size.error.message });
    throw size.error;
  }
  if (size.warning) report({ state: 'connecting', message: `Warning: ${size.warning}` });
  if (signal?.aborted) {
    const te = new TransferError('cancelled', 'The transfer was cancelled.');
    report({ state: 'failed', message: te.message });
    throw te;
  }

  const now = opts.now ?? (() => new Date());
  const startedAt = now().toISOString();
  let result: TransferResult;
  try {
    report({ state: 'connecting', message: `Connecting (${profile.method})` });
    result = await transport.send(profile, out, report, signal);
    if (!result.ok) throw result.error ?? new TransferError('unknown', 'The transfer did not finish.');
  } catch (err) {
    const te =
      err instanceof TransferError
        ? err
        : new TransferError('unknown', err instanceof Error ? err.message : String(err), err);
    result = { ok: false, bytes: 0, startedAt, finishedAt: now().toISOString(), error: te };
  }

  const remotePath = result.remotePath ?? remoteFilePath(folder, out.name);
  const record: TransferRecord = {
    id: opts.newId ? opts.newId() : crypto.randomUUID(),
    articleId: '',
    kind: 'file',
    deviceId: profile.id === 'adhoc' ? undefined : profile.id,
    deviceName: profile.name,
    method: profile.method,
    format: 'file',
    fileName: out.name,
    remotePath: result.ok ? remotePath : folder,
    bytes: result.ok ? result.bytes || out.blob.size : out.blob.size,
    ok: result.ok,
    error: result.error?.message,
    at: now().toISOString(),
  };
  if (opts.log) {
    try {
      await opts.log(record);
    } catch {
      /* The log is a convenience. A storage error does not fail the send. */
    }
  }

  if (!result.ok) {
    const te = result.error ?? new TransferError('unknown', 'The transfer failed.');
    report({ state: 'failed', message: te.message });
    throw te;
  }
  report({
    state: 'complete',
    fraction: 1,
    bytesSent: result.bytes || out.blob.size,
    bytesTotal: out.blob.size,
    message: `Saved as ${remotePath}`,
  });
  return {
    result,
    file: out,
    folder,
    remotePath,
    warning: size.warning,
    hint: isFirmwareFile(out.name) ? FIRMWARE_HINT : undefined,
    record,
  };
}
