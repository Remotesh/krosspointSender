/**
 * Read the payload that the bookmarklet sends.
 * The bookmarklet puts base64 JSON in `#payload=`, or the user pastes the JSON.
 */
import type { BookmarkletPayload } from '../types';

function decodeBase64Utf8(b64: string): string {
  let s = b64.replace(/\s+/g, '').replace(/-/g, '+').replace(/_/g, '/');
  if (!/^[A-Za-z0-9+/]*={0,2}$/.test(s)) throw new Error('The payload is not JSON and not base64.');
  while (s.length % 4) s += '=';
  const bin = atob(s);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
}

/** Parse a bookmarklet payload from JSON or base64 JSON. Throws on bad input. */
export function parseBookmarkletPayload(raw: string): BookmarkletPayload {
  const s = (raw ?? '').trim();
  if (!s) throw new Error('The payload is empty.');
  let json: string;
  if (s.startsWith('{')) json = s;
  else {
    try {
      json = decodeBase64Utf8(s);
    } catch (e) {
      throw new Error(`The payload cannot be decoded: ${(e as Error).message}`);
    }
  }
  let obj: unknown;
  try {
    obj = JSON.parse(json);
  } catch {
    throw new Error('The payload is not valid JSON.');
  }
  if (!obj || typeof obj !== 'object' || Array.isArray(obj)) throw new Error('The payload must be a JSON object.');
  const o = obj as Record<string, unknown>;
  if (o.v !== undefined && o.v !== 1) throw new Error(`Unknown payload version: ${String(o.v)}.`);
  if (typeof o.html !== 'string' || !o.html.trim()) throw new Error('The payload has no HTML.');
  if (o.url !== undefined && typeof o.url !== 'string') throw new Error('The payload URL must be a string.');
  const url = typeof o.url === 'string' ? o.url : '';
  if (url && !/^https?:\/\//i.test(url)) throw new Error('The payload URL must be http or https.');
  let capturedAt = typeof o.capturedAt === 'string' && !Number.isNaN(Date.parse(o.capturedAt)) ? o.capturedAt : '';
  if (!capturedAt) capturedAt = new Date().toISOString();
  const out: BookmarkletPayload = { v: 1, url, html: o.html, capturedAt };
  if (typeof o.title === 'string' && o.title.trim()) out.title = o.title.trim();
  return out;
}

/**
 * Read `#payload=...` from a location hash.
 * Returns null when the hash has no payload or the payload is bad.
 */
export function readPayloadFromHash(hash: string): BookmarkletPayload | null {
  const h = (hash ?? '').replace(/^#/, '');
  if (!h) return null;
  const part = h.split('&').find((p) => p.startsWith('payload='));
  if (!part) return null;
  let value = part.slice('payload='.length);
  try {
    value = decodeURIComponent(value);
  } catch {
    /* Use the raw value. */
  }
  try {
    return parseBookmarkletPayload(value);
  } catch {
    return null;
  }
}
