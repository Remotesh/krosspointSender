/**
 * Find the reader from the browser.
 *
 * The desktop app finds the reader with a UDP broadcast, a subnet sweep and
 * mDNS. A page cannot send UDP, cannot ask mDNS, and cannot learn its own LAN
 * address. This module does the best a page can do, in this order:
 *
 *   1. helper: when the local helper (the desktop app, 127.0.0.1:8134)
 *      answers, use its GET /discover. That one includes the UDP discovery.
 *   2. mdns:   GET http://crosspoint.local/api/status. The OS resolves the
 *      .local name (Windows 10+, macOS, most Linux).
 *   3. sweep:  GET http://a.b.c.x/api/status for x = 1..254 on candidate /24
 *      prefixes, 32 requests at a time, 700 ms per request.
 *
 * Stages 2 and 3 are plain http to the LAN. An https page (not localhost)
 * may not do that (mixed content), so they are skipped there with a reason.
 * The helper is http://127.0.0.1, which browsers treat as secure; it is
 * still tried, and a failure is quiet.
 *
 * A sweep hit counts only when /api/status parses as CrossPoint
 * (readStatusInfo + isCrossPointStatus, the testConnection parser).
 */
import { TransferError } from '../types';
import { fetchWithTimeout, pageOrigin, pageProtocol } from '../transports/common';
import { isCrossPointStatus, readStatusInfo } from '../transports/crosspointHttp';
import { getHelperTransport } from '../transports/helper';
import { FALLBACKS } from '../transports/explain';

export type FoundSource = 'helper' | 'mdns' | 'sweep';

export interface FoundReader {
  /** Host to put in the profile: an IPv4 address or crosspoint.local. */
  host: string;
  /** Device name, from the helper reply or the "device" field of /api/status. */
  name?: string;
  /** Firmware version from /api/status. */
  version?: string;
  source: FoundSource;
  /** The IPv4 address, when known and different from host (mDNS hit). */
  ip?: string;
}

export type FindStage = 'helper' | 'mdns' | 'sweep' | 'done';

export interface FindProgress {
  stage: FindStage;
  /** Plain text for the progress line. */
  message: string;
  /** Sweep only: the prefix in work, e.g. "192.168.0". */
  prefix?: string;
  /** Sweep only: 0..1 of the current prefix. */
  fraction?: number;
  /** All readers found so far. */
  found: FoundReader[];
}

export interface FindReaderOptions {
  signal?: AbortSignal;
  /** User prefix, e.g. "192.168.1" (also "192.168.1.x", "192.168.1.0/24", "192.168.1.25"). */
  subnet?: string;
  /** Hosts of the saved device profiles. Each private IPv4 host adds its /24. */
  profileHosts?: string[];
  /** Helper base URL. Default: the settings value (getHelperTransport().helperUrl()). Pass false to skip the helper. */
  helperUrl?: string | false;
  /** Page protocol and origin. Default: location. For tests. */
  protocol?: string;
  origin?: string;
  /** Timeouts in ms. */
  helperTimeoutMs?: number;
  helperDiscoverTimeoutMs?: number;
  mdnsTimeoutMs?: number;
  probeTimeoutMs?: number;
  /** Requests in flight during the sweep. Max 32. */
  concurrency?: number;
  /**
   * false (default): the sweep runs only when the helper and mDNS found
   * nothing, and it stops after the first prefix with a hit. A prefix the
   * user typed is always swept. true: sweep every candidate prefix.
   */
  thorough?: boolean;
}

export interface FindReaderResult {
  readers: FoundReader[];
  cancelled: boolean;
  /** Why a stage did not run or failed, in plain words. */
  notes: string[];
  /** The prefixes the sweep finished (or started, when cancelled). */
  swept: string[];
}

export const MDNS_HOST = 'crosspoint.local';
export const HELPER_TIMEOUT_MS = 3000;
/** The desktop /discover waits 1.5 s for UDP replies, then may try crosspoint.local. */
export const HELPER_DISCOVER_TIMEOUT_MS = 6000;
export const MDNS_TIMEOUT_MS = 2000;
export const PROBE_TIMEOUT_MS = 700;
export const SWEEP_CONCURRENCY = 32;
/** Common home-router prefixes. 192.168.4 is the reader's own hotspot. */
export const DEFAULT_PREFIXES = ['192.168.0', '192.168.1', '192.168.4', '10.0.0', '192.168.2', '192.168.50'];

/** The reason shown when an https page cannot reach the LAN. */
export const HTTPS_SKIP_REASON =
  'This page uses https, so the browser blocks plain http requests to the reader (mixed content). ' +
  'crosspoint.local and the subnet sweep were skipped. ' +
  FALLBACKS.localhost +
  ' Use "Save an offline copy" in the Device panel.';

const IPV4_RE = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

function octetsOk(parts: string[]): boolean {
  return parts.every((p) => /^\d{1,3}$/.test(p) && Number(p) <= 255);
}

/** True for 10/8, 172.16/12 and 192.168/16. */
export function isPrivatePrefix(prefix: string): boolean {
  const [a, b] = prefix.split('.').map(Number);
  return a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168);
}

/**
 * Reads a user subnet: "192.168.1", "192.168.1.", "192.168.1.x",
 * "192.168.1.0/24" or "192.168.1.25" all give "192.168.1".
 * Returns undefined for anything else.
 */
export function parsePrefix(input: string | undefined): string | undefined {
  if (!input) return undefined;
  const s = input.trim().replace(/\/24$/, '').replace(/\.(x|\*)?$/i, '');
  const parts = s.split('.');
  if (parts.length === 4) parts.pop();
  if (parts.length !== 3 || !octetsOk(parts)) return undefined;
  return parts.map(Number).join('.');
}

/** The /24 prefix of an IPv4 host, or undefined. */
export function hostPrefix(host: string): string | undefined {
  const m = IPV4_RE.exec(host.trim());
  if (!m || !octetsOk(m.slice(1, 5))) return undefined;
  return [m[1], m[2], m[3]].map(Number).join('.');
}

/**
 * The sweep prefixes in order: the user prefix, then the /24 of each saved
 * profile host (private ranges only), then DEFAULT_PREFIXES. No repeats.
 */
export function candidatePrefixes(subnet?: string, profileHosts: string[] = []): string[] {
  const out: string[] = [];
  const add = (p: string | undefined): void => {
    if (p && isPrivatePrefix(p) && !out.includes(p)) out.push(p);
  };
  add(parsePrefix(subnet));
  for (const h of profileHosts) add(hostPrefix(h));
  for (const p of DEFAULT_PREFIXES) add(p);
  return out;
}

/** True when this page may not send plain http to the LAN (https, not localhost, not file://). */
export function lanBlocked(protocol: string = pageProtocol(), origin: string = pageOrigin()): boolean {
  if (protocol !== 'https:') return false;
  return !/^https:\/\/(localhost|[^/]*\.localhost|127\.\d+\.\d+\.\d+|\[::1\])(:\d+)?(\/|$)/i.test(origin);
}

function str(v: unknown): string | undefined {
  return typeof v === 'string' && v.trim() !== '' ? v.trim() : undefined;
}

function cancelled(signal?: AbortSignal): boolean {
  return !!signal?.aborted;
}

/** GET http://host/api/status. The info when it parses as CrossPoint, else undefined. Never throws, except on cancel. */
async function probeStatus(host: string, timeoutMs: number, signal?: AbortSignal): Promise<Record<string, unknown> | undefined> {
  try {
    const res = await fetchWithTimeout(`http://${host}/api/status`, { method: 'GET', host, timeoutMs }, signal);
    if (!res.ok) return undefined;
    const info = await readStatusInfo(res);
    return isCrossPointStatus(info) ? info : undefined;
  } catch (err) {
    if (err instanceof TransferError && err.code === 'cancelled' && cancelled(signal)) throw err;
    return undefined;
  }
}

/** Stage 1. Returns the readers the helper found, or a note. */
async function askHelper(
  url: string,
  opts: FindReaderOptions,
): Promise<{ readers: FoundReader[]; note?: string }> {
  const host = (() => {
    try {
      return new URL(url).hostname;
    } catch {
      return '127.0.0.1';
    }
  })();
  const t = opts.helperTimeoutMs ?? HELPER_TIMEOUT_MS;
  try {
    const st = await fetchWithTimeout(`${url}/status`, { method: 'GET', host, timeoutMs: t }, opts.signal);
    if (!st.ok) return { readers: [], note: 'The desktop helper does not answer.' };
  } catch (err) {
    if (cancelled(opts.signal)) throw err;
    return { readers: [], note: 'The desktop helper does not answer.' };
  }
  try {
    const res = await fetchWithTimeout(
      `${url}/discover`,
      { method: 'GET', host, timeoutMs: opts.helperDiscoverTimeoutMs ?? HELPER_DISCOVER_TIMEOUT_MS },
      opts.signal,
    );
    if (!res.ok) return { readers: [], note: 'The helper runs, but it cannot discover readers (the Python helper has no /discover; the desktop app has).' };
    const data = (await res.json()) as { ok?: boolean; devices?: unknown };
    const list = Array.isArray(data?.devices) ? data.devices : [];
    const readers: FoundReader[] = [];
    for (const d of list as Array<Record<string, unknown>>) {
      const ip = str(d?.ip);
      if (!ip) continue;
      readers.push({ host: ip, name: str(d.hostname), source: 'helper' });
    }
    return { readers };
  } catch (err) {
    if (cancelled(opts.signal)) throw err;
    return { readers: [], note: 'The helper runs, but its discovery did not answer.' };
  }
}

/**
 * Finds CrossPoint readers. Cancel with opts.signal: the promise then
 * resolves with what was found so far and cancelled: true.
 */
export async function findReader(
  opts: FindReaderOptions = {},
  onProgress: (p: FindProgress) => void = () => {},
): Promise<FindReaderResult> {
  const signal = opts.signal;
  const readers: FoundReader[] = [];
  const notes: string[] = [];
  const swept: string[] = [];
  const emit = (p: Omit<FindProgress, 'found'>): void => {
    try {
      onProgress({ ...p, found: readers.slice() });
    } catch {
      /* A UI error must not stop the search. */
    }
  };
  /** Adds a reader unless its host (or its ip) is known. True when added. */
  const add = (r: FoundReader): boolean => {
    const keys = [r.host, r.ip].filter(Boolean).map((k) => k!.toLowerCase());
    if (readers.some((x) => [x.host, x.ip].some((k) => k && keys.includes(k.toLowerCase())))) return false;
    readers.push(r);
    return true;
  };
  const result = (isCancelled: boolean): FindReaderResult => {
    emit({ stage: 'done', message: isCancelled ? 'Cancelled.' : readers.length ? `Found ${readers.length} reader${readers.length === 1 ? '' : 's'}.` : 'No reader found.' });
    return { readers, cancelled: isCancelled, notes, swept };
  };

  try {
    // 1. Helper.
    if (opts.helperUrl !== false) {
      if (cancelled(signal)) return result(true);
      emit({ stage: 'helper', message: 'Asking the desktop helper…' });
      let url = opts.helperUrl;
      if (url === undefined) {
        try {
          url = await getHelperTransport().helperUrl();
        } catch {
          url = undefined;
        }
      }
      if (url) {
        const h = await askHelper(url.replace(/\/+$/, ''), opts);
        for (const r of h.readers) add(r);
        if (h.note) notes.push(h.note);
      }
    }

    // Stages 2 and 3 need plain http to the LAN.
    if (lanBlocked(opts.protocol ?? pageProtocol(), opts.origin ?? pageOrigin())) {
      notes.push(HTTPS_SKIP_REASON);
      return result(cancelled(signal));
    }

    // 2. mDNS name.
    if (cancelled(signal)) return result(true);
    emit({ stage: 'mdns', message: `Trying ${MDNS_HOST}…` });
    const info = await probeStatus(MDNS_HOST, opts.mdnsTimeoutMs ?? MDNS_TIMEOUT_MS, signal);
    if (info) {
      const ip = str(info.ip);
      add({
        host: MDNS_HOST,
        ip: ip && ip !== MDNS_HOST ? ip : undefined,
        name: str(info.device),
        version: str(info.version),
        source: 'mdns',
      });
    } else {
      notes.push(`${MDNS_HOST} did not answer.`);
    }

    // 3. Subnet sweep.
    const userPrefix = parsePrefix(opts.subnet);
    if (opts.subnet?.trim() && !userPrefix) notes.push(`"${opts.subnet.trim()}" is not a subnet. Type the first three numbers, e.g. 192.168.1.`);
    else if (userPrefix && !isPrivatePrefix(userPrefix)) notes.push(`${userPrefix}.x is not a private (home network) range, so it was not swept.`);
    const prefixes = candidatePrefixes(opts.subnet, opts.profileHosts);
    if (!opts.thorough && readers.length > 0) {
      // Found already. Sweep only a prefix the user typed.
      prefixes.splice(0, prefixes.length, ...(userPrefix && isPrivatePrefix(userPrefix) ? [userPrefix] : []));
    }
    const limit = Math.max(1, Math.min(SWEEP_CONCURRENCY, opts.concurrency ?? SWEEP_CONCURRENCY));
    const timeoutMs = opts.probeTimeoutMs ?? PROBE_TIMEOUT_MS;
    for (const prefix of prefixes) {
      if (cancelled(signal)) return result(true);
      swept.push(prefix);
      let next = 1;
      let done = 0;
      let hits = 0;
      emit({ stage: 'sweep', message: `Sweeping ${prefix}.x (0%)`, prefix, fraction: 0 });
      const worker = async (): Promise<void> => {
        while (next <= 254 && !cancelled(signal)) {
          const host = `${prefix}.${next++}`;
          let st: Record<string, unknown> | undefined;
          try {
            st = await probeStatus(host, timeoutMs, signal);
          } catch {
            return; // cancelled
          }
          done++;
          if (st && add({ host, name: str(st.device), version: str(st.version), source: 'sweep' })) hits++;
          const fraction = done / 254;
          emit({ stage: 'sweep', message: `Sweeping ${prefix}.x (${Math.floor(fraction * 100)}%)`, prefix, fraction });
        }
      };
      await Promise.all(Array.from({ length: limit }, worker));
      if (cancelled(signal)) return result(true);
      if (hits > 0 && !opts.thorough) break;
    }
    return result(false);
  } catch (err) {
    if (cancelled(signal)) return result(true);
    throw err;
  }
}
