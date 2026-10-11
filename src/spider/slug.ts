/**
 * File-name slugs for pack pages.
 */

/** Latin letters that NFD does not split into a base letter and a mark. */
const SPECIAL_FOLDS: Record<string, string> = {
  ß: 'ss', æ: 'ae', œ: 'oe', ø: 'o', đ: 'd', ð: 'd', þ: 'th', ł: 'l', ı: 'i', ŋ: 'n', ħ: 'h',
};

/**
 * Make a file-name slug from a title.
 * Rules: lowercase, fold common Latin accents to ASCII, change each run of
 * other characters to "_", trim "_", keep max 48 characters.
 * The result is never empty ("page").
 * Uniqueness inside a pack is the job of the caller.
 */
export function packSlug(title: string): string {
  let s = (title ?? '').toLowerCase();
  s = s.replace(/[ßæœøđðþłıŋħ]/g, (c) => SPECIAL_FOLDS[c] ?? c);
  s = s.normalize('NFD').replace(/[̀-ͯ]/g, '');
  s = s.replace(/[^a-z0-9]+/g, '_').replace(/^_+|_+$/g, '');
  s = s.slice(0, 48).replace(/_+$/g, '');
  return s || 'page';
}
