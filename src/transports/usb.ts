/**
 * USB folder write with the File System Access API.
 * Put the X4 Pro in USB Drive mode. The SD card shows as a drive.
 * The user picks the drive root (or any folder) once. The handle is kept in
 * IndexedDB with the key "usb:<profileId>". On send we ask for permission
 * again when needed, create the sub-folders of targetFolder, write the file,
 * then read it back and compare the size.
 * Chromium browsers (Chrome, Edge, Opera) support this API. Firefox and Safari do not.
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
import { deleteHandle, loadHandle, saveHandle } from '../storage';
import { folderSegments, remoteFilePath, runSend, safeEmitter } from './common';

type PermissionMode = { mode: 'read' | 'readwrite' };

/** Parts of the File System Access API that lib.dom does not declare. */
interface PermissionedHandle {
  queryPermission?(d: PermissionMode): Promise<PermissionState>;
  requestPermission?(d: PermissionMode): Promise<PermissionState>;
}

interface WritableLike {
  write(data: Blob): Promise<void>;
  close(): Promise<void>;
  abort?(): Promise<void>;
}

interface PickerWindow {
  showDirectoryPicker?(opts?: { mode?: 'read' | 'readwrite'; id?: string }): Promise<FileSystemDirectoryHandle>;
}

export const USB_PICKER_ID = 'krosspoint-x4';

export function usbHandleKey(profileId: string): string {
  return `usb:${profileId}`;
}

function pickerWindow(): PickerWindow {
  return globalThis as unknown as PickerWindow;
}

export function isUsbSupported(): boolean {
  return typeof pickerWindow().showDirectoryPicker === 'function';
}

function unsupported(): TransferError {
  return new TransferError(
    'unsupported',
    'This browser cannot write to a folder. Use Chrome or Edge on a desktop, or download the file and copy it by hand.',
  );
}

function mapFsError(err: unknown): TransferError {
  if (err instanceof TransferError) return err;
  const name = (err as { name?: string } | null)?.name;
  const msg = (err as { message?: string } | null)?.message ?? String(err);
  switch (name) {
    case 'AbortError':
      return new TransferError('cancelled', 'You closed the folder picker. No folder was chosen.', err);
    case 'NotAllowedError':
    case 'SecurityError':
      return new TransferError('auth', 'The browser did not give write permission for the folder.', err);
    case 'NotFoundError':
      return new TransferError('not-found', 'The folder is gone. Connect the reader in USB Drive mode and choose the folder again.', err);
    case 'NoModificationAllowedError':
    case 'InvalidStateError':
      return new TransferError('device', 'The drive is read-only or busy. Eject nothing yet, and try again.', err);
    case 'QuotaExceededError':
      return new TransferError('device', 'The drive is full.', err);
    case 'TypeMismatchError':
      return new TransferError('device', 'A file has the same name as a folder in the target path.', err);
    default:
      return new TransferError('unknown', `The folder write failed: ${msg}`, err);
  }
}

/**
 * Shows the folder picker and stores the handle for the profile.
 * Call it from a click handler. The picker needs a user gesture.
 * Returns the folder name.
 */
export async function pickUsbFolder(profileId: string): Promise<string> {
  const w = pickerWindow();
  if (typeof w.showDirectoryPicker !== 'function') throw unsupported();
  try {
    const handle = await w.showDirectoryPicker({ mode: 'readwrite', id: USB_PICKER_ID });
    await saveHandle(usbHandleKey(profileId), handle);
    return handle.name;
  } catch (err) {
    throw mapFsError(err);
  }
}

/** Removes the stored folder for the profile. */
export async function forgetUsbFolder(profileId: string): Promise<void> {
  await deleteHandle(usbHandleKey(profileId));
}

/** The stored folder name for the profile, or undefined. */
export async function usbFolderName(profileId: string): Promise<string | undefined> {
  try {
    return (await loadHandle(usbHandleKey(profileId)))?.name;
  } catch {
    return undefined;
  }
}

async function ensurePermission(handle: FileSystemDirectoryHandle, request: boolean): Promise<PermissionState> {
  const h = handle as unknown as PermissionedHandle;
  const desc: PermissionMode = { mode: 'readwrite' };
  let state: PermissionState = 'granted';
  if (typeof h.queryPermission === 'function') state = await h.queryPermission(desc);
  if (state !== 'granted' && request && typeof h.requestPermission === 'function') {
    state = await h.requestPermission(desc);
  }
  return state;
}

export class UsbTransport implements TransportAdapter {
  readonly method = 'usb' as const;

  isSupported(): boolean {
    return isUsbSupported();
  }

  limitations(): string[] {
    const out = [
      'Put the reader in USB Drive mode and connect it with a cable.',
      'Choose the drive (the SD card root) once. The browser remembers it for this profile.',
      'The browser asks for write permission again in a new session.',
    ];
    if (!this.isSupported()) {
      out.unshift('This browser has no File System Access API. Use Chrome or Edge on a desktop.');
    }
    return out;
  }

  async testConnection(profile: DeviceProfile): Promise<ConnectionTestResult> {
    const t0 = Date.now();
    if (!this.isSupported()) return { ok: false, error: unsupported() };
    try {
      const handle = await loadHandle(usbHandleKey(profile.id));
      if (!handle) {
        return {
          ok: false,
          error: new TransferError('not-found', 'No USB folder is chosen for this device. Click "Choose USB folder".'),
        };
      }
      const state = await ensurePermission(handle, false);
      return {
        ok: true,
        latencyMs: Date.now() - t0,
        info: { folder: handle.name, permission: state, targetFolder: profile.targetFolder },
      };
    } catch (err) {
      return { ok: false, error: mapFsError(err) };
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
    const checkAbort = () => {
      if (signal?.aborted) throw new TransferError('cancelled', 'The transfer was cancelled.');
    };

    return runSend(file, onProgress, undefined, async () => {
      if (!this.isSupported()) throw unsupported();
      emit({ state: 'connecting', message: 'Opening the USB folder' });
      try {
        let root = await loadHandle(usbHandleKey(profile.id));
        if (!root) {
          // No stored folder. Try the picker. This works while the click gesture is active.
          await pickUsbFolder(profile.id);
          root = await loadHandle(usbHandleKey(profile.id));
          if (!root) throw new TransferError('not-found', 'No USB folder is chosen for this device.');
        }
        const state = await ensurePermission(root, true);
        if (state !== 'granted') {
          throw new TransferError('auth', 'The browser did not give write permission for the USB folder. Click Send again and allow it.');
        }
        checkAbort();

        let dir = root;
        for (const seg of folderSegments(profile.targetFolder)) {
          dir = await dir.getDirectoryHandle(seg, { create: true });
        }
        checkAbort();

        emit({ state: 'uploading', fraction: 0, bytesSent: 0, bytesTotal: total, message: `Writing ${file.name}` });
        const fh = await dir.getFileHandle(file.name, { create: true });
        const writable = (await (fh as unknown as { createWritable(): Promise<WritableLike> }).createWritable()) as WritableLike;
        try {
          await writable.write(file.blob);
          await writable.close();
        } catch (err) {
          try {
            await writable.abort?.();
          } catch {
            /* ignore */
          }
          throw err;
        }
        emit({ state: 'uploading', fraction: 1, bytesSent: total, bytesTotal: total });

        emit({ state: 'verifying', fraction: 1, bytesSent: total, bytesTotal: total, message: 'Checking the file size' });
        const written = await fh.getFile();
        if (written.size !== total) {
          throw new TransferError('device', `The drive has ${written.size} bytes, but the file has ${total} bytes. Write it again.`);
        }
        return `${root.name}${remoteFilePath(profile.targetFolder, file.name)}`;
      } catch (err) {
        throw mapFsError(err);
      }
    });
  }
}

export const usbTransport = new UsbTransport();
