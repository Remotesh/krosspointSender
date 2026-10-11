/**
 * Remote folder browse and create, for the device wizard folder step.
 * crosspoint-http: GET /api/files?path=<dir> and POST /mkdir (form name, path).
 * webdav: PROPFIND with Depth 1 (multistatus XML) and MKCOL.
 * Other methods have no remote folder list. They reject with 'unsupported'.
 */
import { TransferError, type DeviceProfile } from '../types';
import {
  basicAuthHeader,
  baseUrl,
  encodePath,
  fetchWithTimeout,
  folderNoTrailingSlash,
  folderSegments,
  httpError,
  normalizeFolder,
} from './common';

export interface RemoteEntry {
  name: string;
  size: number;
  isDirectory: boolean;
}

/** True when the method can list and create remote folders. */
export function canBrowseRemote(method: DeviceProfile['method']): boolean {
  return method === 'crosspoint-http' || method === 'webdav';
}

function unsupported(method: string): TransferError {
  return new TransferError('unsupported', `The method ${method} cannot list folders on the device. Type the folder path.`);
}

function authHeaders(profile: DeviceProfile): Record<string, string> {
  const auth = basicAuthHeader(profile);
  return auth ? { Authorization: auth } : {};
}

/** A folder name must be one path segment without reserved characters. */
function checkName(name: string): string {
  const n = name.trim();
  if (!n || n === '.' || n === '..' || /[\/:*?"<>|\x00-\x1f]/.test(n)) {
    throw new TransferError('device', `"${name}" is not a valid folder name.`);
  }
  return n;
}

/** Lists one remote folder. Folders first, then files, each sorted by name. */
export async function listRemoteFolder(profile: DeviceProfile, path: string): Promise<RemoteEntry[]> {
  let out: RemoteEntry[];
  if (profile.method === 'crosspoint-http') out = await listCrossPoint(profile, path);
  else if (profile.method === 'webdav') out = await listWebDav(profile, path);
  else throw unsupported(profile.method);
  return out.sort((a, b) =>
    a.isDirectory !== b.isDirectory ? (a.isDirectory ? -1 : 1) : a.name.localeCompare(b.name),
  );
}

/** Creates the folder `name` inside `path` on the device. An existing folder is not an error. */
export async function makeRemoteFolder(profile: DeviceProfile, path: string, name: string): Promise<void> {
  const n = checkName(name);
  const host = profile.host;
  if (profile.method === 'crosspoint-http') {
    const form = new URLSearchParams({ name: n, path: folderNoTrailingSlash(path) });
    const res = await fetchWithTimeout(`${baseUrl(profile)}/mkdir`, { method: 'POST', body: form, host });
    if (!res.ok) throw httpError(res.status, `Creating the folder ${n}`, await res.text().catch(() => ''));
    return;
  }
  if (profile.method === 'webdav') {
    const target = `${normalizeFolder(path)}${n}/`;
    const res = await fetchWithTimeout(`${baseUrl(profile)}${encodePath(target)}`, {
      method: 'MKCOL',
      headers: authHeaders(profile),
      host,
    });
    // 405 means the folder exists.
    if (res.ok || res.status === 405) return;
    throw httpError(res.status, `Creating the folder ${target}`, await res.text().catch(() => ''));
  }
  throw unsupported(profile.method);
}

async function listCrossPoint(profile: DeviceProfile, path: string): Promise<RemoteEntry[]> {
  const dir = folderNoTrailingSlash(path);
  const res = await fetchWithTimeout(`${baseUrl(profile)}/api/files?path=${encodeURIComponent(dir)}`, {
    method: 'GET',
    host: profile.host,
  });
  if (!res.ok) throw httpError(res.status, `Listing ${dir}`);
  let data: unknown;
  try {
    data = await res.json();
  } catch {
    throw new TransferError('device', 'The device returned a file list that is not JSON.');
  }
  if (!Array.isArray(data)) throw new TransferError('device', 'The device returned a file list that is not an array.');
  return data
    .filter((e): e is { name: string; size?: unknown; isDirectory?: unknown } => !!e && typeof e.name === 'string')
    .map((e) => ({ name: e.name, size: typeof e.size === 'number' ? e.size : 0, isDirectory: e.isDirectory === true }));
}

async function listWebDav(profile: DeviceProfile, path: string): Promise<RemoteEntry[]> {
  const dir = normalizeFolder(path);
  const body =
    '<?xml version="1.0" encoding="utf-8"?><D:propfind xmlns:D="DAV:"><D:prop>' +
    '<D:resourcetype/><D:getcontentlength/></D:prop></D:propfind>';
  const res = await fetchWithTimeout(`${baseUrl(profile)}${encodePath(dir)}`, {
    method: 'PROPFIND',
    headers: { ...authHeaders(profile), Depth: '1', 'Content-Type': 'application/xml; charset=utf-8' },
    body,
    host: profile.host,
  });
  if (res.status !== 207 && !res.ok) throw httpError(res.status, `Listing ${dir}`);
  return parsePropfind(await res.text(), dir);
}

/** Last path segment of a WebDAV href, decoded. Empty for the root. */
function hrefPath(href: string): string {
  let p = href.trim();
  try {
    p = new URL(p, 'http://x').pathname;
  } catch {
    /* Keep the raw value. */
  }
  try {
    p = decodeURIComponent(p);
  } catch {
    /* Keep the encoded value. */
  }
  return normalizeFolder(p);
}

/**
 * Parses a WebDAV multistatus body. Skips the entry for the folder itself.
 * Works with any namespace prefix.
 */
export function parsePropfind(xml: string, folder: string): RemoteEntry[] {
  const doc = new DOMParser().parseFromString(xml, 'application/xml');
  if (doc.getElementsByTagName('parsererror').length) {
    throw new TransferError('device', 'The device returned WebDAV XML that does not parse.');
  }
  const self = normalizeFolder(folder);
  const out: RemoteEntry[] = [];
  const responses = Array.from(doc.getElementsByTagNameNS('*', 'response'));
  for (const r of responses) {
    const href = r.getElementsByTagNameNS('*', 'href')[0]?.textContent ?? '';
    if (!href) continue;
    const p = hrefPath(href);
    if (p === self) continue;
    const segs = folderSegments(p);
    const name = segs[segs.length - 1];
    if (!name) continue;
    const isDirectory = r.getElementsByTagNameNS('*', 'collection').length > 0;
    const len = Number(r.getElementsByTagNameNS('*', 'getcontentlength')[0]?.textContent ?? '0');
    out.push({ name, size: Number.isFinite(len) ? len : 0, isDirectory });
  }
  return out;
}
