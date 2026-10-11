/**
 * Markdown and plain text to HTML.
 */
import { marked } from 'marked';
import { sanitizeHtml } from './sanitize';

/** Convert Markdown (GitHub flavour) to sanitized HTML. */
export function markdownToHtml(md: string): string {
  const raw = marked.parse(md ?? '', { async: false, gfm: true, breaks: false }) as string;
  return sanitizeHtml(raw);
}

export function escapeHtml(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

/**
 * Convert plain text to HTML.
 * A blank line starts a new paragraph. Hard-wrapped prose joins into one line.
 * Short lines (poems, addresses) keep their line breaks.
 */
export function textToHtml(txt: string): string {
  const norm = (txt ?? '').replace(/^﻿/, '').replace(/\r\n?/g, '\n').replace(/\t/g, '    ');
  const paras = norm.split(/\n[ \t ]*\n+/);
  const out: string[] = [];
  for (const para of paras) {
    const lines = para.split('\n').map((l) => l.replace(/\s+$/, '')).filter((l) => l.trim() !== '');
    if (!lines.length) continue;
    const avg = lines.reduce((a, l) => a + l.length, 0) / lines.length;
    const hardWrapped = lines.length > 1 && avg >= 50;
    const joined = hardWrapped
      ? lines.map((l) => escapeHtml(l.trim())).join(' ')
      : lines.map((l) => escapeHtml(l.trim())).join('<br>');
    out.push(`<p>${joined}</p>`);
  }
  return out.join('\n');
}
