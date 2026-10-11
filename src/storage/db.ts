/**
 * IndexedDB access for k-rosspointSender.
 * Database "krosspoint", version 2.
 * Stores: articles, devices, settings, handles, packs.
 * Version 2 adds the "packs" store (key id, index updatedAt).
 */
import { openDB, type DBSchema, type IDBPDatabase } from 'idb';
import type { AppSettings, DeviceProfile, LibraryEntry, Pack } from '../types';

export const DB_NAME = 'krosspoint';
export const DB_VERSION = 2;

export interface KrosspointDB extends DBSchema {
  articles: {
    key: string;
    value: LibraryEntry;
    indexes: { sourceUrl: string; updatedAt: string };
  };
  devices: {
    key: string;
    value: DeviceProfile;
  };
  settings: {
    key: string;
    value: AppSettings;
  };
  handles: {
    key: string;
    value: FileSystemDirectoryHandle;
  };
  packs: {
    key: string;
    value: StoredPack;
    indexes: { updatedAt: string };
  };
}

/** A pack in the library. The image bytes are optional (cache). */
export interface StoredPack {
  pack: Pack;
  id: string;
  updatedAt: string;
  /** Last output filename. */
  fileName?: string;
  /** Cached image bytes keyed by ArticleImage.src. */
  imageBlobs?: Record<string, Blob>;
  /** Number of sends of this pack. */
  sends?: { at: string; ok: boolean; deviceName?: string; method: string; error?: string }[];
}

let dbPromise: Promise<IDBPDatabase<KrosspointDB>> | null = null;

/** Opens the database once and reuses the connection. */
export function getDb(): Promise<IDBPDatabase<KrosspointDB>> {
  if (!dbPromise) {
    if (typeof indexedDB === 'undefined') {
      return Promise.reject(new Error('IndexedDB is not available in this browser.'));
    }
    dbPromise = openDB<KrosspointDB>(DB_NAME, DB_VERSION, {
      upgrade(db) {
        if (!db.objectStoreNames.contains('articles')) {
          const articles = db.createObjectStore('articles', { keyPath: 'article.id' });
          articles.createIndex('sourceUrl', 'article.sourceUrl');
          articles.createIndex('updatedAt', 'updatedAt');
        }
        if (!db.objectStoreNames.contains('devices')) {
          db.createObjectStore('devices', { keyPath: 'id' });
        }
        if (!db.objectStoreNames.contains('settings')) {
          db.createObjectStore('settings');
        }
        if (!db.objectStoreNames.contains('handles')) {
          db.createObjectStore('handles');
        }
        // Version 2: packs. The upgrade from version 1 keeps all other stores.
        if (!db.objectStoreNames.contains('packs')) {
          const packs = db.createObjectStore('packs', { keyPath: 'id' });
          packs.createIndex('updatedAt', 'updatedAt');
        }
      },
    }).catch((err) => {
      dbPromise = null;
      throw err;
    });
  }
  return dbPromise;
}

/** Closes the connection. The next call to getDb() opens it again. */
export async function closeDb(): Promise<void> {
  if (!dbPromise) return;
  const p = dbPromise;
  dbPromise = null;
  try {
    (await p).close();
  } catch {
    /* The open failed. Nothing to close. */
  }
}
