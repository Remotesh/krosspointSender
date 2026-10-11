/**
 * Filename rules. The output must be safe on FAT32/exFAT SD cards,
 * Windows, macOS and Linux.
 */
import type { Article, FilenameRules, OutputFormat } from '../types';
import { DEFAULT_FILENAME_RULES, isoDay } from './util';

// Reserved characters and control characters.
const RESERVED = /[<>:"/\\|?*\u0000-\u001F\u007F-\u009F]/g;
// Windows device names. A file must not use them as its base name.
const DEVICE_NAMES = /^(con|prn|aux|nul|com[0-9]|lpt[0-9])$/i;

function trimEdges(s: string, extra: string): string {
  const chars = new Set(['.', ' ', ...extra.split('')]);
  const arr = Array.from(s);
  let a = 0;
  let b = arr.length;
  while (a < b && chars.has(arr[a])) a++;
  while (b > a && chars.has(arr[b - 1])) b--;
  return arr.slice(a, b).join('');
}

/**
 * Makes a safe base filename (without extension).
 * It removes reserved and control characters, collapses whitespace, trims
 * dots and spaces, and applies the length limit. It never returns "".
 */
export function sanitizeFilename(name: string, rules?: Partial<FilenameRules>): string {
  const r = { ...DEFAULT_FILENAME_RULES, ...(rules ?? {}) };
  const maxLength = Number.isFinite(r.maxLength) && r.maxLength > 0 ? Math.floor(r.maxLength) : 80;
  // The replacement must itself be safe.
  const repl = (r.spaceReplacement ?? '').replace(RESERVED, '').replace(/\s+/g, '');

  let s = (name ?? '').normalize('NFC').replace(RESERVED, ' ');
  s = s.replace(/\s+/g, ' ');
  s = trimEdges(s, '');
  if (r.lowercase) s = s.toLowerCase();
  if (repl) {
    s = s.replace(/ /g, repl);
    // Collapse runs of the replacement, e.g. "a - b" with "-" gives "a---b".
    const esc = repl.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
    s = s.replace(new RegExp(`(?:${esc}){2,}`, 'g'), repl);
  }
  // Cut on code points, so we never split a surrogate pair.
  const cps = Array.from(s);
  if (cps.length > maxLength) s = cps.slice(0, maxLength).join('');
  s = trimEdges(s, repl);
  if (DEVICE_NAMES.test(s)) s = `_${s}`;
  return s || 'untitled';
}

const EXT: Record<OutputFormat, string> = {
  epub: 'epub',
  html: 'html',
  txt: 'txt',
  md: 'md',
  zip: 'zip',
};

/**
 * Makes the final filename with extension from the template.
 * Tokens: {title} {author} {site} {date} {published} {captured} {year} {lang} {id}.
 * An empty token removes its separators too.
 */
export function makeFilename(article: Article, format: OutputFormat, rules: FilenameRules): string {
  const date = isoDay(article.publishedAt) || isoDay(article.capturedAt);
  const tokens: Record<string, string> = {
    title: article.title ?? '',
    author: article.author ?? '',
    site: article.siteName ?? '',
    date,
    published: isoDay(article.publishedAt),
    captured: isoDay(article.capturedAt),
    year: date.slice(0, 4),
    lang: article.language ?? '',
    id: (article.id ?? '').slice(0, 8),
  };
  const template = rules?.template?.trim() || '{title}';
  let s = template.replace(/\{(\w+)\}/g, (_m, k: string) => tokens[k.toLowerCase()] ?? '');
  // Clean up separators that an empty token left behind.
  s = s
    .replace(/\(\s*\)|\[\s*\]/g, ' ')
    .replace(/(\s*[-_–—,;·|]\s*){2,}/g, (m) => (m.includes(' ') ? ' - ' : m.trim()[0] ?? ' '))
    .replace(/^\s*[-_–—,;·|]+\s*/, '')
    .replace(/\s*[-_–—,;·|]+\s*$/, '');
  if (!s.trim()) s = article.title || 'untitled';
  return `${sanitizeFilename(s, rules)}.${EXT[format]}`;
}
