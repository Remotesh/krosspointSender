import { describe, expect, it } from 'vitest';
import { openDB } from 'idb';
import type { Pack } from '../../src/types';
import {
  clearLibrary,
  closeDb,
  deletePack,
  getArticle,
  getDb,
  getPack,
  getPackEntry,
  listPackEntries,
  listPacks,
  savePack,
  saveArticle,
  listArticles,
} from '../../src/storage';

function pack(id: string, title = `Pack ${id}`): Pack {
  return {
    id,
    kind: 'wikipedia',
    title,
    rootKey: 'Steam engine',
    rootUrl: 'https://en.wikipedia.org/wiki/Steam_engine',
    limits: {
      depth: 1,
      maxArticles: 5,
      maxImagesPerArticle: 2,
      maxTotalEpubSizeMB: 25,
      requestDelayMs: 0,
      includeReferences: false,
      includeInfobox: false,
      sameDomainOnly: true,
    },
    pages: [
      {
        candidate: {
          key: 'Steam engine',
          title: 'Steam engine',
          url: 'https://en.wikipedia.org/wiki/Steam_engine',
          depth: 0,
          linkCount: 0,
          selected: true,
        },
        state: 'queued',
      },
    ],
    externalLinks: [],
    createdAt: '2026-10-10T00:00:00.000Z',
    updatedAt: '2026-10-10T00:00:00.000Z',
  };
}

const oldArticle = (id: string) => ({
  article: { id, title: 'Old', capturedAt: '', tags: [], bodyHtml: '', toc: [], images: [], wordCount: 0, inputKind: 'text' as const },
  fileName: 'old.epub',
  lastFormat: 'epub' as const,
  transfers: [],
  savedAt: '2026-01-01',
  updatedAt: '2026-01-01',
});

const tick = () => new Promise((r) => setTimeout(r, 5));

describe('packs store', () => {
  it('upgrades a version 1 database and keeps its articles', async () => {
    await closeDb();
    // Make an old version 1 database with one article.
    const old = await openDB('krosspoint', 1, {
      upgrade(db) {
        const a = db.createObjectStore('articles', { keyPath: 'article.id' });
        a.createIndex('sourceUrl', 'article.sourceUrl');
        a.createIndex('updatedAt', 'updatedAt');
        db.createObjectStore('devices', { keyPath: 'id' });
        db.createObjectStore('settings');
        db.createObjectStore('handles');
      },
    });
    await old.put('articles', oldArticle('old-1'));
    old.close();
    const db = await getDb();
    expect(db.version).toBe(2);
    expect(Array.from(db.objectStoreNames)).toContain('packs');
    expect(Array.from(db.transaction('packs').store.indexNames)).toContain('updatedAt');
    expect((await getArticle('old-1'))?.article.title).toBe('Old');
  });

  it('round trips a pack with cached images and a send log', async () => {
    const img = new Blob(['jpeg'], { type: 'image/jpeg' });
    await savePack(pack('p1'), { fileName: 'p1.epub', imageBlobs: { 'https://x/img.jpg': img } });
    const back = await getPack('p1');
    expect(back?.title).toBe('Pack p1');
    expect(back?.pages[0].candidate.key).toBe('Steam engine');
    // A second save without images keeps the cache and appends a send.
    await savePack({ ...pack('p1'), title: 'Renamed' }, { send: { ok: true, method: 'crosspoint-http', deviceName: 'X4' } });
    const e = await getPackEntry('p1');
    expect(e?.pack.title).toBe('Renamed');
    expect(e?.fileName).toBe('p1.epub');
    expect(Object.keys(e?.imageBlobs ?? {})).toEqual(['https://x/img.jpg']);
    expect(e?.sends?.length).toBe(1);
    expect(e?.sends?.[0].ok).toBe(true);
  });

  it('lists packs newest first and deletes one', async () => {
    await savePack(pack('p2'));
    await tick();
    await savePack(pack('p3'));
    const ids = (await listPacks()).map((p) => p.id);
    expect(ids.indexOf('p3')).toBeLessThan(ids.indexOf('p2'));
    expect((await listPackEntries()).length).toBe(ids.length);
    await deletePack('p2');
    expect(await getPack('p2')).toBeUndefined();
  });

  it('rejects a pack without an id', async () => {
    await expect(savePack({ ...pack('x'), id: '' })).rejects.toThrow();
  });

  it('clearLibrary removes articles and packs', async () => {
    await saveArticle(oldArticle('a1'));
    await savePack(pack('p4'));
    await clearLibrary();
    expect(await listPacks()).toEqual([]);
    expect(await listArticles()).toEqual([]);
  });
});
