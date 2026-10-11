import { beforeEach, describe, expect, it } from 'vitest';
import type { Article, LibraryEntry, TransferRecord } from '../../src/types';
import {
  saveArticle,
  getArticle,
  listArticles,
  deleteArticle,
  clearLibrary,
  addTransfer,
  findBySourceUrl,
  exportLibraryJson,
  getDb,
} from '../../src/storage';

function article(id: string, over: Partial<Article> = {}): Article {
  return {
    id,
    title: `Article ${id}`,
    capturedAt: '2026-10-10T00:00:00.000Z',
    sourceUrl: `https://example.com/${id}`,
    tags: [],
    bodyHtml: '<p>Hello</p>',
    toc: [],
    images: [],
    wordCount: 1,
    inputKind: 'url',
    ...over,
  };
}

function entry(id: string, updatedAt: string, over: Partial<LibraryEntry> = {}): LibraryEntry {
  return {
    article: article(id),
    fileName: `${id}.epub`,
    lastFormat: 'epub',
    transfers: [],
    savedAt: updatedAt,
    updatedAt,
    ...over,
  };
}

function blobText(b: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const fr = new FileReader();
    fr.onload = () => resolve(String(fr.result));
    fr.onerror = () => reject(fr.error);
    fr.readAsText(b);
  });
}

beforeEach(async () => {
  await clearLibrary();
});

describe('storage', () => {
  it('creates the krosspoint database with all stores', async () => {
    const db = await getDb();
    expect(db.name).toBe('krosspoint');
    expect(db.version).toBe(2);
    expect(Array.from(db.objectStoreNames).sort()).toEqual(['articles', 'devices', 'handles', 'packs', 'settings']);
  });

  it('round-trips an article', async () => {
    const e = entry('a1', '2026-10-10T01:00:00.000Z');
    await saveArticle(e);
    expect(await getArticle('a1')).toEqual(e);
    expect(await getArticle('missing')).toBeUndefined();
  });

  it('lists newest first', async () => {
    await saveArticle(entry('old', '2026-10-01T00:00:00.000Z'));
    await saveArticle(entry('new', '2026-10-09T00:00:00.000Z'));
    await saveArticle(entry('mid', '2026-10-05T00:00:00.000Z'));
    expect((await listArticles()).map((e) => e.article.id)).toEqual(['new', 'mid', 'old']);
  });

  it('deletes one and clears all', async () => {
    await saveArticle(entry('a', '2026-10-01T00:00:00.000Z'));
    await saveArticle(entry('b', '2026-10-02T00:00:00.000Z'));
    await deleteArticle('a');
    expect((await listArticles()).map((e) => e.article.id)).toEqual(['b']);
    await clearLibrary();
    expect(await listArticles()).toEqual([]);
  });

  it('appends transfer records', async () => {
    await saveArticle(entry('t', '2026-10-01T00:00:00.000Z'));
    const rec: TransferRecord = {
      id: 'r1',
      articleId: 't',
      method: 'webdav',
      format: 'epub',
      fileName: 't.epub',
      bytes: 10,
      ok: true,
      at: '2026-10-10T00:00:00.000Z',
    };
    await addTransfer('t', rec);
    await addTransfer('t', { ...rec, id: 'r2', ok: false, error: 'network' });
    const got = await getArticle('t');
    expect(got!.transfers.map((r) => r.id)).toEqual(['r1', 'r2']);
    expect(got!.updatedAt > '2026-10-01T00:00:00.000Z').toBe(true);
    await expect(addTransfer('nope', rec)).rejects.toThrow();
  });

  it('finds an entry by source URL', async () => {
    await saveArticle(entry('s', '2026-10-01T00:00:00.000Z'));
    expect((await findBySourceUrl('https://example.com/s'))?.article.id).toBe('s');
    expect(await findBySourceUrl('https://example.com/none')).toBeUndefined();
  });

  it('exports JSON without image blobs', async () => {
    await saveArticle(entry('x', '2026-10-01T00:00:00.000Z', { imageBlobs: {} }));
    const blob = await exportLibraryJson();
    expect(blob.type).toBe('application/json');
    const doc = JSON.parse(await blobText(blob));
    expect(doc.count).toBe(1);
    expect(doc.articles[0].article.id).toBe('x');
    expect('imageBlobs' in doc.articles[0]).toBe(false);
  });
});
