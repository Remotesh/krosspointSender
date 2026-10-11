import { afterEach, describe, expect, it, vi } from 'vitest';
import { TransferError } from '../../src/types';
import { listRemoteFolder, makeRemoteFolder, parsePropfind } from '../../src/transports';
import { profile, stubFetch } from './mocks';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

const MULTISTATUS = `<?xml version="1.0" encoding="utf-8"?>
<d:multistatus xmlns:d="DAV:">
  <d:response><d:href>/Books/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>
  <d:response><d:href>/Books/K-rosspoint/</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>
  <d:response><d:href>http://192.168.1.50/Books/My%20Book.epub</d:href><d:propstat><d:prop><d:resourcetype/><d:getcontentlength>1234</d:getcontentlength></d:prop></d:propstat></d:response>
  <d:response><d:href>/Books/Alpha</d:href><d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype></d:prop></d:propstat></d:response>
</d:multistatus>`;

describe('listRemoteFolder', () => {
  it('crosspoint-http: GET /api/files with the folder path and maps entries', async () => {
    const calls = stubFetch(() => ({
      status: 200,
      body: JSON.stringify([
        { name: 'b.epub', size: 10, isDirectory: false, isEpub: true },
        { name: 'Zed', size: 0, isDirectory: true },
        { name: 'Alpha', isDirectory: true },
      ]),
    }));
    const out = await listRemoteFolder(profile({ method: 'crosspoint-http' }), '/Books/');
    expect(calls[0].method).toBe('GET');
    expect(calls[0].url).toBe('http://192.168.1.50:80/api/files?path=%2FBooks');
    expect(out).toEqual([
      { name: 'Alpha', size: 0, isDirectory: true },
      { name: 'Zed', size: 0, isDirectory: true },
      { name: 'b.epub', size: 10, isDirectory: false },
    ]);
  });

  it('crosspoint-http: a bad status and bad JSON become TransferErrors', async () => {
    stubFetch(() => ({ status: 500, body: 'boom' }));
    await expect(listRemoteFolder(profile({ method: 'crosspoint-http' }), '/')).rejects.toBeInstanceOf(TransferError);
    stubFetch(() => ({ status: 200, body: 'not json' }));
    const err = await listRemoteFolder(profile({ method: 'crosspoint-http' }), '/').catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('device');
  });

  it('webdav: PROPFIND with Depth 1 and parses the multistatus body', async () => {
    const calls = stubFetch(() => ({ status: 207, body: MULTISTATUS }));
    const out = await listRemoteFolder(profile({ method: 'webdav', username: 'u', password: 'p' }), '/Books');
    expect(calls[0].method).toBe('PROPFIND');
    expect(calls[0].url).toBe('http://192.168.1.50:80/Books/');
    expect(calls[0].headers.Depth).toBe('1');
    expect(calls[0].headers.Authorization).toMatch(/^Basic /);
    expect(out).toEqual([
      { name: 'Alpha', size: 0, isDirectory: true },
      { name: 'K-rosspoint', size: 0, isDirectory: true },
      { name: 'My Book.epub', size: 1234, isDirectory: false },
    ]);
  });

  it('parsePropfind skips the folder itself at the root', () => {
    const xml =
      '<multistatus xmlns="DAV:"><response><href>/</href></response>' +
      '<response><href>/Books/</href><propstat><prop><resourcetype><collection/></resourcetype></prop></propstat></response></multistatus>';
    expect(parsePropfind(xml, '/')).toEqual([{ name: 'Books', size: 0, isDirectory: true }]);
    expect(() => parsePropfind('<not xml', '/')).toThrow(TransferError);
  });

  it('other methods reject with unsupported', async () => {
    const err = await listRemoteFolder(profile({ method: 'usb' }), '/').catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('unsupported');
  });

  it('a network failure becomes a TransferError', async () => {
    stubFetch(() => new TypeError('Failed to fetch'));
    const err = await listRemoteFolder(profile({ method: 'webdav' }), '/').catch((e) => e);
    expect(err).toBeInstanceOf(TransferError);
    expect(err.code).toBe('network');
  });
});

describe('makeRemoteFolder', () => {
  it('crosspoint-http: POST /mkdir with form name and path', async () => {
    const calls = stubFetch(() => ({ status: 200, body: 'ok' }));
    await makeRemoteFolder(profile({ method: 'crosspoint-http' }), '/Books/', 'New One');
    expect(calls[0].method).toBe('POST');
    expect(calls[0].url).toBe('http://192.168.1.50:80/mkdir');
    const form = calls[0].body as URLSearchParams;
    expect(form.get('name')).toBe('New One');
    expect(form.get('path')).toBe('/Books');
  });

  it('webdav: MKCOL on the new folder path; 405 (exists) is OK', async () => {
    const calls = stubFetch(() => ({ status: 405 }));
    await makeRemoteFolder(profile({ method: 'webdav' }), '/Books', 'My Folder');
    expect(calls[0].method).toBe('MKCOL');
    expect(calls[0].url).toBe('http://192.168.1.50:80/Books/My%20Folder/');
  });

  it('rejects a bad name and a failing device', async () => {
    stubFetch(() => ({ status: 500, body: 'no' }));
    await expect(makeRemoteFolder(profile({ method: 'webdav' }), '/', 'a/b')).rejects.toBeInstanceOf(TransferError);
    await expect(makeRemoteFolder(profile({ method: 'crosspoint-http' }), '/', 'ok')).rejects.toBeInstanceOf(TransferError);
    await expect(makeRemoteFolder(profile({ method: 'download' }), '/', 'ok')).rejects.toMatchObject({ code: 'unsupported' });
  });
});
