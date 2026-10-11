/**
 * Public API of the spider module (Spider Pack mode). See docs/ARCHITECTURE.md,
 * "Addendum 2026-10-10: Spider Pack mode".
 */
export { normalizeWikipediaTitle, scanWikipediaLinks, extractBodyLinks } from './wikipediaSpider';
export { scanWebLinks } from './webSpider';
export { fetchPackPages, newPack, DEFAULT_PACK_LIMITS } from './pack';
export { rewriteLinks } from './linkRewriter';
export { packSlug } from './slug';
export { webKey, canonicalTitle } from './linkFilter';
