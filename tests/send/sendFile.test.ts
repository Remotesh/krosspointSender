import { beforeEach, describe, expect, it } from 'vitest';
import type {
  DeviceProfile,
  OutputFile,
  TransferMethod,
  TransferProgress,
  TransferRecord,
  TransferResult,
  TransportAdapter,
} from '../../src/types';
import { TransferError } from '../../src/types';
import {
  FIRMWARE_HINT,
  MAX_SEND_FILE_BYTES,
  checkFileSize,
  cleanFileName,
  isArticleFile,
  mimeFor,
  profileWithFolder,
  sendRawFile,
  toOutputFile,
} from '../../src/send';
import { clearTransferLog, getDb, listArticles, listTransferLog, logTransfer } from '../../src/storage';

function profile(over: Partial<DeviceProfile> = {}): DeviceProfile {
  return {
    id: 'dev1',
    name: 'X4 Pro',
    firmware: 'crosspoint',
    host: '192.168.1.50',
    port: 80,
    method: 'crosspoint-http',
    targetFolder: '/Books/Inbox/',
    defaultFormat: 'epub',
    createdAt: '2026-10-10T00:00:00.000Z',
    updatedAt: '2026-10-10T00:00:00.000Z',
    ...over,
  };
}

interface SendCall {
  profile: DeviceProfile;
  file: OutputFile;
  signal?: AbortSignal;
}

/** A fake transport. It records each call and answers with the behaviour given. */
function fakeTransport(
  method: TransferMethod = 'crosspoint-http',
  behave: (c: SendCall, onProgress: (p: TransferProgress) => void) => Promise<TransferResult> = async (c, onProgress) => {
    onProgress({ state: 'uploading', fraction: 0.5, bytesSent: c.file.blob.size / 2, bytesTotal: c.file.blob.size });
    return {
      ok: true,
      remotePath: `${c.profile.targetFolder}${c.file.name}`,
      bytes: c.file.blob.size,
      startedAt: 'a',
      finishedAt: 'b',
    };
  },
): TransportAdapter & { calls: SendCall[] } {
  const calls: SendCall[] = [];
  return {
    method,
    calls,
    isSupported: () => true,
    limitations: () => [],
    testConnection: async () => ({ ok: true }),
    send: (p, file, onProgress, signal) => {
      const c = { profile: p, file, signal };
      calls.push(c);
      return behave(c, onProgress);
    },
  };
}

/** A Blob that reports a size without holding the bytes. */
function sizedFile(name: string, size: number, type = ''): File {
  const f = new File(['x'], name, { type });
  Object.defineProperty(f, 'size', { value: size });
  return f;
}

const MB = 1024 * 1024;

describe('toOutputFile', () => {
  it('wraps a File as an OutputFile with format "file", the right mime and size', () => {
    const f = new File([new Uint8Array(1234)], 'firmware.bin');
    const out = toOutputFile(f);
    expect(out).toEqual({ name: 'firmware.bin', mime: 'application/octet-stream', blob: f, format: 'file' });
    expect(out.blob.size).toBe(1234);
  });

  it('uses the edited name and its mime', () => {
    const f = new File(['abc'], 'book.bin');
    const out = toOutputFile(f, '  My Book.epub ');
    expect(out.name).toBe('My Book.epub');
    expect(out.mime).toBe('application/epub+zip');
  });

  it('falls back to the browser type, then octet-stream', () => {
    expect(mimeFor('a.weird', 'text/x-weird')).toBe('text/x-weird');
    expect(mimeFor('noext')).toBe('application/octet-stream');
    expect(mimeFor('a.BMP')).toBe('image/bmp');
  });

  it('strips path parts and refuses an empty name', () => {
    expect(cleanFileName('C:\\fake\\path\\firmware.bin')).toBe('firmware.bin');
    expect(cleanFileName('../x/../y.txt')).toBe('y.txt');
    expect(() => cleanFileName('   ')).toThrow(TransferError);
    expect(() => toOutputFile(new File(['a'], 'a.bin'), '/')).toThrow(/file name/);
  });
});

describe('folder handling', () => {
  it('normalises the folder like an article send', () => {
    expect(profileWithFolder(profile(), '/').targetFolder).toBe('/');
    expect(profileWithFolder(profile(), 'Books//Firmware').targetFolder).toBe('/Books/Firmware/');
    expect(profileWithFolder(profile(), '\\a\\b\\').targetFolder).toBe('/a/b/');
  });

  it('keeps the device folder when the field is empty', () => {
    const p = profile();
    expect(profileWithFolder(p, '').targetFolder).toBe('/Books/Inbox/');
    expect(profileWithFolder(p, undefined)).toBe(p);
  });

  it('sends to the chosen folder and does not change the saved profile', async () => {
    const t = fakeTransport();
    const p = profile();
    const out = await sendRawFile({ profile: p, file: new File(['x'], 'firmware.bin'), folder: '/', transport: t });
    expect(t.calls).toHaveLength(1);
    expect(t.calls[0].profile.targetFolder).toBe('/');
    expect(t.calls[0].file.format).toBe('file');
    expect(p.targetFolder).toBe('/Books/Inbox/');
    expect(out.folder).toBe('/');
    expect(out.remotePath).toBe('/firmware.bin');
  });

  it('uses the device default folder when no folder is given', async () => {
    const t = fakeTransport();
    const out = await sendRawFile({ profile: profile(), file: new File(['x'], 'notes.pdf'), transport: t });
    expect(t.calls[0].profile.targetFolder).toBe('/Books/Inbox/');
    expect(out.remotePath).toBe('/Books/Inbox/notes.pdf');
    expect(out.hint).toBeUndefined();
  });
});

describe('sendRawFile', () => {
  it('reports progress and complete, and gives the firmware hint for .bin', async () => {
    const seen: TransferProgress[] = [];
    const out = await sendRawFile({
      profile: profile(),
      file: new File([new Uint8Array(10)], 'firmware.bin'),
      folder: '/',
      transport: fakeTransport(),
      onProgress: (p) => seen.push(p),
    });
    expect(seen.map((p) => p.state)).toEqual(['connecting', 'uploading', 'complete']);
    expect(seen.at(-1)?.message).toBe('Saved as /firmware.bin');
    expect(out.hint).toBe(FIRMWARE_HINT);
    expect(FIRMWARE_HINT).toContain('Settings > System > SD Card Firmware Update');
  });

  it('passes the cancel signal to the transport and maps a cancel', async () => {
    const ctrl = new AbortController();
    const t = fakeTransport('crosspoint-http', async (c) => {
      expect(c.signal).toBe(ctrl.signal);
      ctrl.abort();
      throw new TransferError('cancelled', 'The transfer was cancelled.');
    });
    await expect(
      sendRawFile({ profile: profile(), file: new File(['x'], 'a.bin'), transport: t, signal: ctrl.signal }),
    ).rejects.toMatchObject({ code: 'cancelled' });
  });

  it('does not call the transport when already cancelled', async () => {
    const ctrl = new AbortController();
    ctrl.abort();
    const t = fakeTransport();
    await expect(
      sendRawFile({ profile: profile(), file: new File(['x'], 'a.bin'), transport: t, signal: ctrl.signal }),
    ).rejects.toMatchObject({ code: 'cancelled' });
    expect(t.calls).toHaveLength(0);
  });

  it('keeps the transport error (overwrite rules stay the transport\'s)', async () => {
    const t = fakeTransport('crosspoint-http', async () => {
      throw new TransferError('exists', 'File already exists: firmware.bin');
    });
    const seen: TransferProgress[] = [];
    await expect(
      sendRawFile({ profile: profile(), file: new File(['x'], 'firmware.bin'), transport: t, onProgress: (p) => seen.push(p) }),
    ).rejects.toMatchObject({ code: 'exists' });
    expect(seen.at(-1)).toMatchObject({ state: 'failed', message: 'File already exists: firmware.bin' });
  });

  it('treats a result with ok false as a failure', async () => {
    const t = fakeTransport('webdav', async () => ({
      ok: false,
      bytes: 0,
      startedAt: 'a',
      finishedAt: 'b',
      error: new TransferError('device', 'HTTP 500'),
    }));
    await expect(sendRawFile({ profile: profile({ method: 'webdav' }), file: new File(['x'], 'a.bin'), transport: t })).rejects.toMatchObject({
      code: 'device',
    });
  });
});

describe('size rules', () => {
  it('refuses files over 256 MB on any transport, before the transport', async () => {
    const t = fakeTransport('usb');
    const big = sizedFile('huge.bin', MAX_SEND_FILE_BYTES + 1);
    await expect(sendRawFile({ profile: profile({ method: 'usb' }), file: big, transport: t })).rejects.toMatchObject({
      code: 'too-large',
      message: expect.stringContaining('256 MB'),
    });
    expect(t.calls).toHaveLength(0);
    expect(checkFileSize('ok.bin', MAX_SEND_FILE_BYTES, 'usb')).toEqual({});
  });

  it('refuses an empty file', () => {
    expect(checkFileSize('empty.bin', 0, 'webdav').error?.message).toMatch(/empty/);
  });

  it('BLE: refuses over 16 MB and says Wi-Fi is better for firmware', async () => {
    const t = fakeTransport('ble');
    const fw = sizedFile('firmware.bin', 17 * MB);
    await expect(sendRawFile({ profile: profile({ method: 'ble' }), file: fw, transport: t })).rejects.toMatchObject({
      code: 'too-large',
      message: expect.stringMatching(/16 MB.*Wi-Fi is better/),
    });
    expect(t.calls).toHaveLength(0);
  });

  it('BLE: warns over 3 MB but sends', async () => {
    const t = fakeTransport('ble');
    const seen: TransferProgress[] = [];
    const out = await sendRawFile({
      profile: profile({ method: 'ble' }),
      file: sizedFile('firmware.bin', 5 * MB),
      transport: t,
      onProgress: (p) => seen.push(p),
    });
    expect(t.calls).toHaveLength(1);
    expect(out.warning).toMatch(/Wi-Fi is better/);
    expect(seen[0].message).toMatch(/^Warning: firmware\.bin is 5\.0 MB/);
  });

  it('BLE: a small firmware file sends with a Wi-Fi tip; a small other file has no warning', () => {
    expect(checkFileSize('firmware.bin', 1 * MB, 'ble').warning).toMatch(/Wi-Fi is better/);
    expect(checkFileSize('cover.bmp', 1 * MB, 'ble')).toEqual({});
  });

  it('Wi-Fi: no warning for a large firmware file', () => {
    expect(checkFileSize('firmware.bin', 20 * MB, 'crosspoint-http')).toEqual({});
  });
});

describe('transfer history', () => {
  beforeEach(async () => {
    await getDb();
    await clearTransferLog();
  });

  it('records a successful send with kind "file" and no library entry', async () => {
    const before = (await listArticles()).length;
    const out = await sendRawFile({
      profile: profile(),
      file: new File([new Uint8Array(42)], 'firmware.bin'),
      folder: '/',
      transport: fakeTransport(),
      log: logTransfer,
      newId: () => 'rec-1',
      now: () => new Date('2026-10-11T10:00:00.000Z'),
    });
    const log = await listTransferLog();
    expect(log).toHaveLength(1);
    const rec: TransferRecord = log[0];
    expect(rec).toEqual({
      id: 'rec-1',
      articleId: '',
      kind: 'file',
      deviceId: 'dev1',
      deviceName: 'X4 Pro',
      method: 'crosspoint-http',
      format: 'file',
      fileName: 'firmware.bin',
      remotePath: '/firmware.bin',
      bytes: 42,
      ok: true,
      error: undefined,
      at: '2026-10-11T10:00:00.000Z',
    });
    expect(out.record).toEqual(rec);
    expect((await listArticles()).length).toBe(before);
  });

  it('records a failed send with the error', async () => {
    const t = fakeTransport('webdav', async () => {
      throw new TransferError('network', 'Could not reach 192.168.1.50.');
    });
    await expect(
      sendRawFile({ profile: profile({ method: 'webdav' }), file: new File(['x'], 'a.bin'), transport: t, log: logTransfer }),
    ).rejects.toThrow();
    const [rec] = await listTransferLog();
    expect(rec).toMatchObject({ kind: 'file', ok: false, method: 'webdav', error: 'Could not reach 192.168.1.50.' });
  });

  it('does not record a refusal before the transport', async () => {
    const log: TransferRecord[] = [];
    await expect(
      sendRawFile({
        profile: profile({ method: 'ble' }),
        file: sizedFile('firmware.bin', 20 * MB),
        transport: fakeTransport('ble'),
        log: async (r) => void log.push(r),
      }),
    ).rejects.toThrow();
    expect(log).toHaveLength(0);
  });

  it('a log error does not fail the send', async () => {
    const out = await sendRawFile({
      profile: profile(),
      file: new File(['x'], 'a.bin'),
      transport: fakeTransport(),
      log: async () => {
        throw new Error('disk full');
      },
    });
    expect(out.result.ok).toBe(true);
  });

  it('lists the log newest first', async () => {
    await logTransfer({ id: 'a', articleId: '', kind: 'file', method: 'usb', format: 'file', fileName: 'a', bytes: 1, ok: true, at: '2026-10-11T01:00:00.000Z' });
    await logTransfer({ id: 'b', articleId: '', kind: 'file', method: 'usb', format: 'file', fileName: 'b', bytes: 1, ok: true, at: '2026-10-11T02:00:00.000Z' });
    expect((await listTransferLog()).map((r) => r.id)).toEqual(['b', 'a']);
    expect((await listTransferLog(1)).map((r) => r.id)).toEqual(['b']);
  });
});

describe('isArticleFile (drop routing)', () => {
  it('keeps HTML, Markdown and TXT on the article path', () => {
    for (const n of ['a.html', 'a.HTM', 'a.xhtml', 'a.md', 'a.markdown', 'a.txt']) {
      expect(isArticleFile({ name: n })).toBe(true);
    }
    expect(isArticleFile({ name: 'README', type: '' })).toBe(true);
  });

  it('sends other files to "Send a file"', () => {
    for (const n of ['firmware.bin', 'book.epub', 'cover.bmp', 'doc.pdf', 'pack.zip']) {
      expect(isArticleFile({ name: n })).toBe(false);
    }
    expect(isArticleFile({ name: 'blob', type: 'application/octet-stream' })).toBe(false);
  });
});
