/** Reading time and page count estimates for the X4 Pro screen. */
import type { Article, StylePreset, StyleSettings } from '../types';
import { countTextWords } from './util';

export const WORDS_PER_MINUTE = 230;

/** Words on one 480x800 page at 100 % font size, per preset. */
export const WORDS_PER_PAGE: Record<StylePreset, number> = {
  compact: 210,
  comfortable: 170,
  large: 120,
  reference: 190,
};

/** Returns the word count. It counts the body text when wordCount is not set. */
export function articleWords(article: Article): number {
  if (article.wordCount > 0) return article.wordCount;
  if (!article.bodyHtml) return 0;
  const doc = new DOMParser().parseFromString(article.bodyHtml, 'text/html');
  return countTextWords(doc.body?.textContent ?? '');
}

/**
 * Estimates reading minutes (230 wpm) and pages.
 * A bigger font gives fewer words per page: wordsPerPage = base * 100 / fontSizePercent.
 */
export function estimateReading(article: Article, style: StyleSettings): { minutes: number; pages: number } {
  const words = articleWords(article);
  const base = WORDS_PER_PAGE[style.preset] ?? WORDS_PER_PAGE.comfortable;
  const pct = Number.isFinite(style.fontSizePercent) && style.fontSizePercent > 0 ? style.fontSizePercent : 100;
  const wordsPerPage = Math.max(1, (base * 100) / pct);
  const minutes = words === 0 ? 0 : Math.max(1, Math.round(words / WORDS_PER_MINUTE));
  const pages = Math.max(1, Math.ceil(words / wordsPerPage));
  return { minutes, pages };
}
