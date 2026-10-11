/**
 * Shared last step: TOC ids, image list, word count, id and dates.
 */
import type { Article, ArticleImage, InputKind } from '../types';
import { parseFragment } from './dom';
import { buildToc, countWords } from './toc';

export function newId(): string {
  const c = (globalThis as { crypto?: Crypto }).crypto;
  if (c && typeof c.randomUUID === 'function') return c.randomUUID();
  const b = new Uint8Array(16);
  if (c?.getRandomValues) c.getRandomValues(b);
  else for (let i = 0; i < 16; i++) b[i] = Math.floor(Math.random() * 256);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  const h = Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  return `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
}

/** List the images of a body in order. Each src is listed once. */
export function collectImages(bodyHtml: string): ArticleImage[] {
  const doc = parseFragment(bodyHtml);
  const seen = new Set<string>();
  const out: ArticleImage[] = [];
  for (const img of Array.from(doc.body.querySelectorAll('img'))) {
    const src = img.getAttribute('src');
    if (!src || seen.has(src)) continue;
    seen.add(src);
    const alt = img.getAttribute('alt');
    out.push(alt ? { src, alt } : { src });
  }
  return out;
}

export interface ArticleParts {
  bodyHtml: string;
  inputKind: InputKind;
  title: string;
  author?: string;
  publishedAt?: string;
  capturedAt?: string;
  sourceUrl?: string;
  siteName?: string;
  excerpt?: string;
  language?: string;
  tags?: string[];
}

/** Build the final Article from the sanitized body and the metadata. */
export function finishArticle(p: ArticleParts): Article {
  const { bodyHtml, toc } = buildToc(p.bodyHtml);
  const a: Article = {
    id: newId(),
    title: p.title.trim() || 'Untitled',
    capturedAt: p.capturedAt ?? new Date().toISOString(),
    language: (p.language || 'en').trim(),
    tags: p.tags ?? [],
    bodyHtml,
    toc,
    images: collectImages(bodyHtml),
    wordCount: countWords(bodyHtml),
    inputKind: p.inputKind,
  };
  if (p.author) a.author = p.author;
  if (p.publishedAt) a.publishedAt = p.publishedAt;
  if (p.sourceUrl) a.sourceUrl = p.sourceUrl;
  if (p.siteName) a.siteName = p.siteName;
  if (p.excerpt) a.excerpt = p.excerpt;
  return a;
}
