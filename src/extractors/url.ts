/**
 * URL helpers and input-kind detection.
 */
import type { InputKind } from '../types';

/** Query parameters that only track the user. We remove them. */
const TRACKING_PARAMS = new Set([
  'fbclid', 'gclid', 'dclid', 'gbraid', 'wbraid', 'msclkid', 'yclid', 'twclid',
  'igshid', 'mc_cid', 'mc_eid', '_hsenc', '_hsmi', 'mkt_tok', 'oly_anon_id',
  'oly_enc_id', 'vero_id', 'wickedid', 'ref_src', 'spm', '__s', 's_cid',
  'cmpid', 'ncid', 'sr_share',
]);

/**
 * Clean a user-typed URL.
 * Trims, adds https://, rejects non-http(s), strips tracking parameters, lowercases the host.
 * Returns null when the input is not a web URL.
 */
export function normalizeUrl(input: string): string | null {
  let s = (input ?? '').trim();
  if (!s || /\s/.test(s)) return null;
  if (s.startsWith('//')) s = 'https:' + s;
  else if (!/^[a-z][a-z0-9+.-]*:\/\//i.test(s)) {
    // A scheme without "//" (mailto:, javascript:, data:) is not a web URL.
    if (/^(javascript|data|mailto|file|about|blob|vbscript|ftp|tel):/i.test(s)) return null;
    s = 'https://' + s;
  }
  let u: URL;
  try {
    u = new URL(s);
  } catch {
    return null;
  }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return null;
  const host = u.hostname.toLowerCase();
  if (!host) return null;
  // A bare word is not a host. Localhost and IPv6 literals are exceptions.
  if (!host.includes('.') && host !== 'localhost' && !host.startsWith('[')) return null;
  for (const key of [...u.searchParams.keys()]) {
    const k = key.toLowerCase();
    if (k.startsWith('utm_') || TRACKING_PARAMS.has(k)) u.searchParams.delete(key);
  }
  let out = u.toString();
  // URL keeps a lone "?" after we delete every parameter. Remove it.
  if (!u.search) out = out.replace(/\?(?=#|$)/, '');
  return out;
}

/** True for a *.wikipedia.org article URL. */
export function isWikipediaUrl(url: string): boolean {
  const n = normalizeUrl(url);
  if (!n) return false;
  const u = new URL(n);
  if (!/(^|\.)wikipedia\.org$/.test(u.hostname)) return false;
  return u.pathname.startsWith('/wiki/') || u.pathname.startsWith('/w/index.php');
}

/** True when the hostname is loopback, private, link-local, or ends in .local. */
export function isPrivateHost(hostname: string): boolean {
  const h = hostname.toLowerCase().replace(/^\[|\]$/g, '');
  if (h === 'localhost' || h.endsWith('.local') || h.endsWith('.localhost') || h.endsWith('.lan') || h.endsWith('.home.arpa')) {
    return true;
  }
  const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/.exec(h);
  if (m) {
    const a = Number(m[1]);
    const b = Number(m[2]);
    if (a === 10 || a === 127 || a === 0) return true;
    if (a === 192 && b === 168) return true;
    if (a === 172 && b >= 16 && b <= 31) return true;
    if (a === 169 && b === 254) return true;
    if (a === 100 && b >= 64 && b <= 127) return true; // Carrier-grade NAT.
    return false;
  }
  if (h.includes(':')) return h === '::1' || /^f[cd]/.test(h) || h.startsWith('fe80');
  return false;
}

const FILE_KINDS: Record<string, InputKind> = {
  md: 'markdown', markdown: 'markdown', mdown: 'markdown', mkd: 'markdown',
  txt: 'text', text: 'text',
  html: 'html', htm: 'html', xhtml: 'html', shtml: 'html',
};

/**
 * Guess what the user gave us: url, wikipedia, html, markdown, bookmarklet or text.
 * A known file extension wins.
 */
export function detectInputKind(text: string, fileName?: string): InputKind {
  if (fileName) {
    const ext = fileName.toLowerCase().split('.').pop() ?? '';
    const k = FILE_KINDS[ext];
    if (k) return k;
  }
  const t = (text ?? '').trim();
  if (!t) return 'text';

  // One token without spaces that parses as a URL.
  if (!/\s/.test(t) && t.length < 4096 && !t.startsWith('<') && !t.startsWith('{')) {
    const n = normalizeUrl(t);
    if (n && (/^https?:\/\//i.test(t) || /^[a-z0-9-]+(\.[a-z0-9-]+)+(:\d+)?(\/|$|\?|#)/i.test(t))) {
      return isWikipediaUrl(n) ? 'wikipedia' : 'url';
    }
  }

  // A bookmarklet payload as JSON.
  if (t.startsWith('{') && /"v"\s*:\s*1/.test(t) && /"html"\s*:/.test(t)) return 'bookmarklet';

  // HTML: a document start, or several common tags.
  if (/^(<\?xml[^>]*>\s*)?(<!doctype html|<html[\s>])/i.test(t)) return 'html';
  const tagCount = (t.match(/<(p|div|article|section|body|head|h[1-6]|span|a|br|ul|ol|li|table|img|em|strong|blockquote|pre)\b[^>]*>/gi) ?? []).length;
  if (tagCount >= 2 || (tagCount === 1 && t.startsWith('<'))) return 'html';

  // Markdown: count typical signals.
  let score = 0;
  if (/^#{1,6}\s+\S/m.test(t)) score += 2;
  if (/^(```|~~~)/m.test(t)) score += 2;
  if (/\[[^\]\n]+\]\([^)\s]+\)/.test(t)) score += 2;
  if (/^\s*[-*+]\s+\S/m.test(t)) score += 1;
  if (/^\s*\d+\.\s+\S/m.test(t)) score += 1;
  if (/(\*\*|__)[^*_\n]+\1/.test(t)) score += 1;
  if (/^>\s+\S/m.test(t)) score += 1;
  if (/^(-{3,}|\*{3,}|_{3,})\s*$/m.test(t)) score += 1;
  if (/`[^`\n]+`/.test(t)) score += 1;
  if (/^\|.+\|\s*$\n^\|?\s*:?-{3,}/m.test(t)) score += 2;
  return score >= 2 ? 'markdown' : 'text';
}
