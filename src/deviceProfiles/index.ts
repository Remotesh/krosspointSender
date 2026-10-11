/**
 * Device profiles. Stored in IndexedDB store "devices".
 */
import type { DeviceProfile, FirmwareType, OutputFormat, TransferMethod } from '../types';
import { getDb } from '../storage/db';

export const TRANSFER_METHODS: readonly TransferMethod[] = [
  'webdav',
  'crosspoint-http',
  'crosspoint-ws',
  'usb',
  'download',
  'opds',
  'ble',
];

export const FIRMWARE_TYPES: readonly FirmwareType[] = ['crosspoint', 'kreader', 'other'];

const OUTPUT_FORMATS: readonly OutputFormat[] = ['epub', 'html', 'txt', 'md', 'zip'];

/** Methods that talk to the device over the LAN. They need a host and a port. */
export const LAN_METHODS: readonly TransferMethod[] = ['webdav', 'crosspoint-http', 'crosspoint-ws'];

type ProfileDefaults = Omit<DeviceProfile, 'id' | 'createdAt' | 'updatedAt'>;

/** Default values for a new profile of a firmware type. */
export function defaultProfile(firmware: FirmwareType): ProfileDefaults {
  switch (firmware) {
    case 'kreader':
      return {
        name: 'K-Reader',
        firmware: 'kreader',
        host: '',
        port: 80,
        method: 'ble',
        targetFolder: '/Books/K-rosspoint/',
        defaultFormat: 'epub',
      };
    case 'other':
      return {
        name: 'Other device',
        firmware: 'other',
        host: '',
        port: 80,
        method: 'webdav',
        targetFolder: '/Books/K-rosspoint/',
        defaultFormat: 'epub',
      };
    case 'crosspoint':
    default:
      return {
        name: 'Xteink X4 Pro (CrossPoint)',
        firmware: 'crosspoint',
        host: 'crosspoint.local',
        port: 80,
        method: 'crosspoint-http',
        targetFolder: '/Books/K-rosspoint/',
        defaultFormat: 'epub',
      };
  }
}

const IPV4_LIKE = /^\d+(\.\d+){3}$/;
const HOST_LABEL = /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$/;

/** True for a host name (RFC 1123 labels) or a dotted IPv4 address. */
export function isValidHost(host: string): boolean {
  if (!host || host.length > 253) return false;
  if (IPV4_LIKE.test(host)) {
    return host.split('.').every((o) => o.length <= 3 && Number(o) <= 255);
  }
  return host.split('.').every((label) => HOST_LABEL.test(label));
}

/** Returns a list of problems. An empty list means the profile is valid. */
export function validateProfile(p: Partial<DeviceProfile>): string[] {
  const problems: string[] = [];
  if (!p.name || !p.name.trim()) problems.push('Enter a name for the device.');

  if (p.method === undefined || !TRANSFER_METHODS.includes(p.method)) {
    problems.push(`Choose a transfer method: ${TRANSFER_METHODS.join(', ')}.`);
  }
  if (p.firmware !== undefined && !FIRMWARE_TYPES.includes(p.firmware)) {
    problems.push(`The firmware must be one of: ${FIRMWARE_TYPES.join(', ')}.`);
  }

  // LAN methods need a host. Other methods check the host only when it is set.
  const host = p.host ?? '';
  const needsHost = p.method !== undefined && LAN_METHODS.includes(p.method);
  if (needsHost || host !== '') {
    if (host === '') {
      problems.push('Enter the device host name or IP address.');
    } else if (/^[a-z][a-z0-9+.-]*:\/\//i.test(host)) {
      problems.push('Remove the scheme (http://) from the host. Enter only the name or the IP address.');
    } else if (/\s/.test(host)) {
      problems.push('The host must not contain spaces.');
    } else if (!isValidHost(host)) {
      problems.push('The host must be a host name (crosspoint.local) or an IPv4 address (192.168.1.50).');
    }
  }

  if (p.port === undefined || !Number.isInteger(p.port) || p.port < 1 || p.port > 65535) {
    problems.push('The port must be a whole number from 1 to 65535.');
  }

  if (!p.targetFolder || !p.targetFolder.startsWith('/')) {
    problems.push('The target folder must start with "/".');
  }

  if (p.defaultFormat !== undefined && !OUTPUT_FORMATS.includes(p.defaultFormat)) {
    problems.push(`The default format must be one of: ${OUTPUT_FORMATS.join(', ')}.`);
  }
  return problems;
}

function newId(): string {
  const c = globalThis.crypto as Crypto | undefined;
  if (c && typeof c.randomUUID === 'function') return c.randomUUID();
  const bytes = new Uint8Array(16);
  if (c && typeof c.getRandomValues === 'function') c.getRandomValues(bytes);
  else for (let i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  const h = Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

function stripUndefined<T extends object>(o: T): Partial<T> {
  const out: Partial<T> = {};
  for (const [k, v] of Object.entries(o)) {
    if (v !== undefined) (out as Record<string, unknown>)[k] = v;
  }
  return out;
}

/** All profiles, oldest first. */
export async function listProfiles(): Promise<DeviceProfile[]> {
  const db = await getDb();
  const all = await db.getAll('devices');
  return all.sort((a, b) => (a.createdAt < b.createdAt ? -1 : a.createdAt > b.createdAt ? 1 : 0));
}

export async function getProfile(id: string): Promise<DeviceProfile | undefined> {
  const db = await getDb();
  return db.get('devices', id);
}

/**
 * Creates or updates a profile. Fills id, timestamps and defaults.
 * When p.id matches a stored profile, the stored values are the base.
 * Throws an Error that lists all problems when the result is not valid.
 */
export async function saveProfile(p: Partial<DeviceProfile> & { name: string }): Promise<DeviceProfile> {
  const db = await getDb();
  const existing = p.id ? await db.get('devices', p.id) : undefined;
  const now = new Date().toISOString();
  const base: ProfileDefaults = existing ?? defaultProfile(p.firmware ?? 'crosspoint');
  const merged: DeviceProfile = {
    ...base,
    ...stripUndefined(p),
    id: existing?.id ?? p.id ?? newId(),
    name: (p.name ?? '').trim(),
    host: (p.host ?? base.host ?? '').trim(),
    createdAt: existing?.createdAt ?? now,
    updatedAt: now,
  };
  const problems = validateProfile(merged);
  if (problems.length > 0) throw new Error(problems.join(' '));
  await db.put('devices', merged);
  return merged;
}

export async function deleteProfile(id: string): Promise<void> {
  const db = await getDb();
  await db.delete('devices', id);
}

/** Sets lastConnectedAt to now. Does nothing when the profile does not exist. */
export async function markConnected(id: string): Promise<void> {
  const db = await getDb();
  const p = await db.get('devices', id);
  if (!p) return;
  const now = new Date().toISOString();
  await db.put('devices', { ...p, lastConnectedAt: now, updatedAt: now });
}
