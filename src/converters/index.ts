/**
 * converters/ public API (Agent B).
 * processImage returns null when it cannot use an image. Without a canvas
 * (for example in jsdom) it passes JPEG and PNG through unchanged and
 * returns null for other types.
 */
export { buildHtml } from './html';
export { buildTxt } from './text';
export { buildMarkdown } from './markdown';
export { buildZip } from './zip';
export { sanitizeFilename, makeFilename } from './filename';
export { processImage, fetchImages } from './images';
export type { ProcessedImage, ProcessImageOptions, ImageMime } from './images';
export { estimateReading } from './reading';
