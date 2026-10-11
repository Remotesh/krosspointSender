/**
 * Spider packs in the library. Store "packs" (key id, index updatedAt).
 * Each record holds the pack, the last output filename, optional cached
 * image bytes and a send log.
 */
import type { Pack } from '../types';
import { getDb, type StoredPack } from './db';

export interface PackExtras {
  fileName?: string;
  /** Cached image bytes keyed by ArticleImage.src. Undefined keeps the old cache. */
  imageBlobs?: Record<string, Blob>;
  /** A send to add to the log. */
  send?: { ok: boolean; deviceName?: string; method: string; error?: string };
}

/** Saves or replaces a pack. Keeps the old image cache and send log when extras do not replace them. */
export async function savePack(pack: Pack, extras: PackExtras = {}): Promise<void> {
  if (!pack?.id) throw new Error('The pack has no id.');
  const db = await getDb();
  const tx = db.transaction('packs', 'readwrite');
  const old = await tx.store.get(pack.id);
  const now = new Date().toISOString();
  const sends = [...(old?.sends ?? [])];
  if (extras.send) sends.push({ at: now, ...extras.send });
  const rec: StoredPack = {
    id: pack.id,
    pack: { ...pack, updatedAt: now },
    updatedAt: now,
    fileName: extras.fileName ?? old?.fileName,
    imageBlobs: extras.imageBlobs ?? old?.imageBlobs,
    sends,
  };
  await tx.store.put(rec);
  await tx.done;
}

/** Returns the pack with this id. */
export async function getPack(id: string): Promise<Pack | undefined> {
  return (await getPackEntry(id))?.pack;
}

/** Returns the full library record of a pack (with cached images and the send log). */
export async function getPackEntry(id: string): Promise<StoredPack | undefined> {
  const db = await getDb();
  return db.get('packs', id);
}

/** Returns all packs, newest first. */
export async function listPacks(): Promise<Pack[]> {
  return (await listPackEntries()).map((e) => e.pack);
}

/** Returns all pack records, newest first. */
export async function listPackEntries(): Promise<StoredPack[]> {
  const db = await getDb();
  const all = await db.getAllFromIndex('packs', 'updatedAt');
  return all.reverse();
}

export async function deletePack(id: string): Promise<void> {
  const db = await getDb();
  await db.delete('packs', id);
}
