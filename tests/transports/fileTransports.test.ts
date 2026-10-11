import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import JSZip from 'jszip';
import { TransferError } from '../../src/types';

// The USB handle objects hold methods. IndexedDB cannot clone them in tests.
// So the storage handle functions use an in-memory map here.
const handles = new Map<string, unknown>();
vi.mock('../../src/storage', () => ({
  saveHandle: async (k: string, h: unknown) => void handles.set(k, h),
  loadHandle: async (k: string) => handles.get(k),
  deleteHandle: async (k: string) => void handles.delete(k),
}));

import { getTransport, pickUsbFolder, buildOpdsCatalog, buildOpdsPackage, usbFolderName } from '../../src/transports';
import { outputFile, profile } from './mocks';

class FakeFile {
  data: Blob = new Blob([]);
  constructor(public name: string) {}
  async createWritable() {
    let pending: Blob | null = null;
    return {
      write: async (b: Blob) => void (pending = b),
      close: async () => void (this.data = pending ?? new Blob([])),
    };
  }
  async getFile() {
    return { size: this.data.size };
  }
}

class FakeDir {
  dirs = new Map<string, FakeDir>();
  files = new Map<string, FakeFile>();
  permission: PermissionState = 'prompt';
  constructor(public name: string) {}
  async getDirectoryHandle(n: string, o?: { create?: boolean }) {
    if (!this.dirs.has(n)) {
      if (!o?.create) throw Object.assign(new Error('nf'), { name: 'NotFoundError' });
      this.dirs.set(n, new FakeDir(n));
    }
    return this.dirs.get(n)!;
  }
  async getFileHandle(n: string, o?: { create?: boolean }) {
    if (!this.files.has(n)) {
      if (!o?.create) throw Object.assign(new Error('nf'), { name: 'NotFoundError' });
      this.files.set(n, new FakeFile(n));
    }
    return this.files.get(n)!;
  }
  async queryPermission() {
    return this.permission;
  }
  async requestPermission() {
    this.permission = 'granted';
    return this.permission;
  }
}

beforeEach(() => handles.clear());
afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('usb', () => {
  it('rejects with unsupported without the File System Access API', async () => {
    expect(getTransport('usb').isSupported()).toBe(false);
    const err = await getTransport('usb').send(profile({ method: 'usb' }), outputFile(), () => {}).catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('unsupported');
  });

  it('picks a folder, writes into sub-folders and verifies the size', async () => {
    const root = new FakeDir('X4DRIVE');
    const picker = vi.fn(async () => root);
    vi.stubGlobal('showDirectoryPicker', picker);
    expect(await pickUsbFolder('dev-1')).toBe('X4DRIVE');
    expect(picker).toHaveBeenCalledWith({ mode: 'readwrite', id: 'krosspoint-x4' });
    expect(handles.has('usb:dev-1')).toBe(true);
    expect(await usbFolderName('dev-1')).toBe('X4DRIVE');

    const file = outputFile();
    const states: string[] = [];
    const res = await getTransport('usb').send(profile({ method: 'usb' }), file, (p) => states.push(p.state));
    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('X4DRIVE/Books/K-rosspoint/My Book.epub');
    const written = root.dirs.get('Books')!.dirs.get('K-rosspoint')!.files.get('My Book.epub')!;
    expect(written.data.size).toBe(file.blob.size);
    expect(root.permission).toBe('granted');
    expect(states).toEqual(expect.arrayContaining(['connecting', 'uploading', 'verifying', 'complete']));
  });

  it('maps a closed picker to cancelled', async () => {
    vi.stubGlobal('showDirectoryPicker', async () => {
      throw Object.assign(new Error('user aborted'), { name: 'AbortError' });
    });
    const err = await pickUsbFolder('dev-2').catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('cancelled');
  });
});

describe('download', () => {
  it('is always supported and completes at once', async () => {
    const create = vi.fn(() => 'blob:fake');
    vi.stubGlobal('URL', Object.assign(URL, { createObjectURL: create, revokeObjectURL: vi.fn() }));
    const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const states: string[] = [];
    const res = await getTransport('download').send(profile({ method: 'download' }), outputFile(), (p) => states.push(p.state));
    expect(res.ok).toBe(true);
    expect(click).toHaveBeenCalledOnce();
    expect(create).toHaveBeenCalledOnce();
    expect(states.at(-1)).toBe('complete');
    expect((await getTransport('download').testConnection(profile())).ok).toBe(true);
  });
});

describe('opds', () => {
  it('builds a valid OPDS 1.2 acquisition feed', () => {
    const xml = buildOpdsCatalog(
      [
        { title: 'Tom & Jerry <1>', author: 'A. "Writer"', fileName: 'Tom & Jerry.epub', bytes: 1234, updated: '2026-10-10T12:00:00Z' },
        { title: 'Second', fileName: 'second.epub', bytes: 10, updated: '2026-10-09T12:00:00Z', language: 'de' },
      ],
      'http://192.168.1.10/books',
    );
    const doc = new DOMParser().parseFromString(xml, 'application/xml');
    expect(doc.getElementsByTagName('parsererror')).toHaveLength(0);
    const feed = doc.documentElement;
    expect(feed.localName).toBe('feed');
    expect(feed.namespaceURI).toBe('http://www.w3.org/2005/Atom');
    expect(feed.getAttribute('xmlns:dc')).toBe('http://purl.org/dc/terms/');
    expect(feed.getAttribute('xmlns:opds')).toBe('http://opds-spec.org/2010/catalog');
    const entries = doc.getElementsByTagNameNS('http://www.w3.org/2005/Atom', 'entry');
    expect(entries).toHaveLength(2);
    const e0 = entries[0];
    const child = (el: Element, ns: string, n: string) => el.getElementsByTagNameNS(ns, n)[0];
    expect(child(e0, 'http://www.w3.org/2005/Atom', 'title').textContent).toBe('Tom & Jerry <1>');
    expect(child(e0, 'http://www.w3.org/2005/Atom', 'id').textContent).toBeTruthy();
    expect(child(e0, 'http://www.w3.org/2005/Atom', 'updated').textContent).toBe('2026-10-10T12:00:00.000Z');
    expect(child(e0, 'http://www.w3.org/2005/Atom', 'name').textContent).toBe('A. "Writer"');
    expect(child(e0, 'http://purl.org/dc/terms/', 'language').textContent).toBe('en');
    expect(child(entries[1], 'http://purl.org/dc/terms/', 'language').textContent).toBe('de');
    const link = Array.from(e0.getElementsByTagNameNS('http://www.w3.org/2005/Atom', 'link')).find(
      (l) => l.getAttribute('rel') === 'http://opds-spec.org/acquisition',
    )!;
    expect(link.getAttribute('type')).toBe('application/epub+zip');
    expect(link.getAttribute('href')).toBe('http://192.168.1.10/books/Tom%20%26%20Jerry.epub');
    expect(link.getAttribute('length')).toBe('1234');
    expect(child(feed, 'http://www.w3.org/2005/Atom', 'updated').textContent).toBe('2026-10-10T12:00:00.000Z');
  });

  it('send downloads a ZIP with catalog.xml and the book', async () => {
    const file = outputFile();
    const zipBlob = await buildOpdsPackage(file);
    const zip = await JSZip.loadAsync(await zipBlob.arrayBuffer());
    expect(Object.keys(zip.files).sort()).toEqual(['My Book.epub', 'catalog.xml']);
    const cat = await zip.file('catalog.xml')!.async('string');
    expect(cat).toContain('href="My%20Book.epub"');

    vi.stubGlobal('URL', Object.assign(URL, { createObjectURL: vi.fn(() => 'blob:z'), revokeObjectURL: vi.fn() }));
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});
    const res = await getTransport('opds').send(profile({ method: 'opds' }), file, () => {});
    expect(res.ok).toBe(true);
    expect(res.remotePath).toBe('My Book-opds.zip');
  });
});
