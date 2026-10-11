/**
 * Article library, file handles and export.
 */
import type { LibraryEntry, TransferRecord } from '../types';
import { getDb } from './db';

/** Saves or replaces one library entry. The key is entry.article.id. */
export async function saveArticle(entry: LibraryEntry): Promise<void> {
  if (!entry?.article?.id) throw new Error('The library entry has no article id.');
  const db = await getDb();
  await db.put('articles', entry);
}

export async function getArticle(id: string): Promise<LibraryEntry | undefined> {
  const db = await getDb();
  return db.get('articles', id);
}

/** Returns all entries, newest first (by updatedAt). */
export async function listArticles(): Promise<LibraryEntry[]> {
  const db = await getDb();
  const all = await db.getAllFromIndex('articles', 'updatedAt');
  return all.reverse();
}

export async function deleteArticle(id: string): Promise<void> {
  const db = await getDb();
  await db.delete('articles', id);
}

/** Deletes all articles and all packs. */
export async function clearLibrary(): Promise<void> {
  const db = await getDb();
  await db.clear('articles');
  await db.clear('packs');
}

/** Appends a transfer record to an entry. Throws when the entry does not exist. */
export async function addTransfer(articleId: string, rec: TransferRecord): Promise<void> {
  const db = await getDb();
  const tx = db.transaction('articles', 'readwrite');
  const entry = await tx.store.get(articleId);
  if (!entry) {
    tx.abort();
    await tx.done.catch(() => undefined);
    throw new Error(`No library entry with id ${articleId}.`);
  }
  entry.transfers = [...(entry.transfers ?? []), { ...rec, articleId }];
  entry.updatedAt = new Date().toISOString();
  await tx.store.put(entry);
  await tx.done;
}

/** Finds the entry for a source URL. Returns the newest match. */
export async function findBySourceUrl(url: string): Promise<LibraryEntry | undefined> {
  if (!url) return undefined;
  const db = await getDb();
  const matches = await db.getAllFromIndex('articles', 'sourceUrl', url);
  if (matches.length === 0) return undefined;
  return matches.sort((a, b) => (a.updatedAt < b.updatedAt ? 1 : -1))[0];
}

export async function saveHandle(key: string, h: FileSystemDirectoryHandle): Promise<void> {
  const db = await getDb();
  await db.put('handles', h, key);
}

export async function loadHandle(key: string): Promise<FileSystemDirectoryHandle | undefined> {
  const db = await getDb();
  return db.get('handles', key);
}

export async function deleteHandle(key: string): Promise<void> {
  const db = await getDb();
  await db.delete('handles', key);
}

/** Exports the library as JSON. Image blobs are not included. */
export async function exportLibraryJson(): Promise<Blob> {
  const entries = await listArticles();
  const articles = entries.map((e) => {
    const { imageBlobs: _omit, ...rest } = e;
    void _omit;
    return rest;
  });
  const doc = {
    app: 'K-rosspointSender',
    format: 'krosspoint-library',
    version: 1,
    exportedAt: new Date().toISOString(),
    count: articles.length,
    articles,
  };
  return new Blob([JSON.stringify(doc, null, 2)], { type: 'application/json' });
}
