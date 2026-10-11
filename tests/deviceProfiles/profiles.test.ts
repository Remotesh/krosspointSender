import { describe, expect, it } from 'vitest';
import {
  defaultProfile,
  deleteProfile,
  getProfile,
  listProfiles,
  markConnected,
  saveProfile,
  validateProfile,
} from '../../src/deviceProfiles';

describe('defaultProfile', () => {
  it('has the CrossPoint defaults', () => {
    const p = defaultProfile('crosspoint');
    expect(p).toMatchObject({
      firmware: 'crosspoint',
      host: 'crosspoint.local',
      port: 80,
      method: 'crosspoint-http',
      targetFolder: '/Books/K-rosspoint/',
      defaultFormat: 'epub',
    });
    expect(defaultProfile('kreader')).toMatchObject({ method: 'ble', targetFolder: '/Books/K-rosspoint/' });
    expect(defaultProfile('other').method).toBe('webdav');
    expect(validateProfile(p)).toEqual([]);
    expect(validateProfile(defaultProfile('kreader'))).toEqual([]);
  });
});

describe('validateProfile', () => {
  const ok = defaultProfile('crosspoint');
  it('accepts host names and IPv4', () => {
    for (const host of ['crosspoint.local', '192.168.4.1', 'reader', 'my-reader.lan']) {
      expect(validateProfile({ ...ok, host })).toEqual([]);
    }
  });
  it('rejects bad values', () => {
    expect(validateProfile({ ...ok, name: ' ' })).toHaveLength(1);
    expect(validateProfile({ ...ok, host: 'http://192.168.1.5' })[0]).toMatch(/scheme/);
    expect(validateProfile({ ...ok, host: 'my reader' })[0]).toMatch(/spaces/);
    expect(validateProfile({ ...ok, host: '300.1.1.1' })).toHaveLength(1);
    expect(validateProfile({ ...ok, host: '192.168.1.5:80' })).toHaveLength(1);
    expect(validateProfile({ ...ok, host: '' })).toHaveLength(1);
    expect(validateProfile({ ...ok, port: 0 })).toHaveLength(1);
    expect(validateProfile({ ...ok, port: 65536 })).toHaveLength(1);
    expect(validateProfile({ ...ok, port: 80.5 })).toHaveLength(1);
    expect(validateProfile({ ...ok, targetFolder: 'Books/' })).toHaveLength(1);
    expect(validateProfile({ ...ok, method: 'ftp' as never })).toHaveLength(1);
    expect(validateProfile({}).length).toBeGreaterThanOrEqual(4);
  });
  it('does not need a host for non-LAN methods', () => {
    expect(validateProfile({ ...ok, method: 'usb', host: '' })).toEqual([]);
  });
});

describe('profile storage', () => {
  it('saves, loads, updates, marks and deletes', async () => {
    const p = await saveProfile({ name: 'My X4' });
    expect(p.id).toMatch(/^[0-9a-f-]{36}$/);
    expect(p.host).toBe('crosspoint.local');
    expect(p.method).toBe('crosspoint-http');
    expect(p.createdAt).toBe(p.updatedAt);
    expect(await getProfile(p.id)).toEqual(p);

    const q = await saveProfile({ id: p.id, name: 'My X4', host: '192.168.1.50', method: 'crosspoint-http' });
    expect(q.id).toBe(p.id);
    expect(q.createdAt).toBe(p.createdAt);
    expect(q.host).toBe('192.168.1.50');
    expect(q.targetFolder).toBe('/Books/K-rosspoint/');

    const k = await saveProfile({ name: 'K', firmware: 'kreader' });
    expect(k.method).toBe('ble');
    expect((await listProfiles()).map((x) => x.id)).toEqual([p.id, k.id]);

    await markConnected(p.id);
    expect((await getProfile(p.id))!.lastConnectedAt).toBeTruthy();

    await deleteProfile(p.id);
    expect(await getProfile(p.id)).toBeUndefined();
    expect((await listProfiles()).map((x) => x.id)).toEqual([k.id]);
  });

  it('rejects an invalid profile', async () => {
    await expect(saveProfile({ name: 'Bad', host: 'http://x', method: 'webdav' })).rejects.toThrow(/scheme/);
  });
});
