/**
 * App settings. Stored in IndexedDB store "settings" with key "app".
 * Stored values merge over DEFAULT_SETTINGS, so new fields get defaults.
 */
import type { AppSettings } from '../types';
import { getDb } from '../storage/db';

const SETTINGS_KEY = 'app';

/** Default helper URL. The helper listens on 127.0.0.1:8134. */
export const DEFAULT_HELPER_URL = 'http://127.0.0.1:8134';

/**
 * Defaults. The style values are the "comfortable" preset from the spec.
 * They are copied here on purpose. Do not import them from src/epub.
 */
export const DEFAULT_SETTINGS: AppSettings = deepFreeze({
  style: {
    preset: 'comfortable',
    fontSizePercent: 100,
    marginEm: 0.8,
    lineHeight: 1.45,
    paragraphSpacingEm: 0.6,
    imageMaxWidthPx: 480,
    grayscaleImages: true,
    keepLinks: true,
    hyphenate: false,
    tableHandling: 'keep',
    imageQuality: 0.85,
    customCss: '',
  },
  filename: {
    template: '{title}',
    maxLength: 80,
    lowercase: false,
    spaceReplacement: '',
  },
  build: {
    includeImages: true,
    includeCover: true,
    includeSourceNote: true,
  },
  extract: {
    keepInfobox: false,
    keepReferences: false,
    keepLinks: true,
    keepImages: true,
    keepTables: true,
  },
  cacheImages: true,
  helperUrl: DEFAULT_HELPER_URL,
  // Same values as DEFAULT_PACK_LIMITS in src/spider. Copied on purpose: settings does not import spider.
  packLimits: {
    depth: 1,
    maxArticles: 20,
    maxImagesPerArticle: 2,
    maxTotalEpubSizeMB: 25,
    requestDelayMs: 500,
    includeReferences: false,
    includeInfobox: false,
    sameDomainOnly: true,
  },
} satisfies AppSettings) as AppSettings;

function deepFreeze<T extends object>(o: T): T {
  for (const v of Object.values(o)) {
    if (typeof v === 'object' && v !== null) deepFreeze(v as object);
  }
  return Object.freeze(o);
}

function isObject(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

/** Returns a deep copy of the defaults. */
export function defaultSettings(): AppSettings {
  return mergeSettings(undefined);
}

/** Merges a stored (possibly partial or old) value over the defaults. Returns a new object. */
export function mergeSettings(stored: unknown): AppSettings {
  const s = isObject(stored) ? stored : {};
  const d = DEFAULT_SETTINGS;
  const sub = <K extends 'style' | 'filename' | 'build' | 'extract' | 'packLimits'>(k: K): AppSettings[K] =>
    ({ ...d[k], ...(isObject(s[k]) ? (s[k] as object) : {}) }) as AppSettings[K];
  return {
    ...d,
    ...s,
    style: sub('style'),
    filename: sub('filename'),
    build: sub('build'),
    extract: sub('extract'),
    packLimits: sub('packLimits'),
  } as AppSettings;
}

/** Loads settings. Stored values merge over the defaults. */
export async function loadSettings(): Promise<AppSettings> {
  try {
    const db = await getDb();
    const stored = await db.get('settings', SETTINGS_KEY);
    return mergeSettings(stored);
  } catch {
    // No IndexedDB (for example a locked-down private window). Use the defaults.
    return defaultSettings();
  }
}

export async function saveSettings(s: AppSettings): Promise<void> {
  const db = await getDb();
  await db.put('settings', mergeSettings(s), SETTINGS_KEY);
}

/** Removes stored settings and returns the defaults. */
export async function resetSettings(): Promise<AppSettings> {
  const db = await getDb();
  await db.delete('settings', SETTINGS_KEY);
  return defaultSettings();
}
