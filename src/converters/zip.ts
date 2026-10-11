/** ZIP bundle: EPUB + HTML + TXT + Markdown + article.json. */
import JSZip from 'jszip';
import type { Article, BuildOptions, OutputFile } from '../types';
import { buildEpubBytes } from '../epub/build';
import { sanitizeFilename } from './filename';
import { buildHtml } from './html';
import { buildMarkdown } from './markdown';
import { buildTxt } from './text';
import { bytesToBlob } from './util';

export async function buildZip(article: Article, opts: BuildOptions, images?: Map<string, Blob>): Promise<OutputFile> {
  const base = sanitizeFilename(article.title);
  const epub = await buildEpubBytes(article, opts, images);
  const zip = new JSZip();
  zip.file(`${base}.epub`, epub.bytes, { binary: true, compression: 'STORE' });
  zip.file(`${base}.html`, buildHtml(article, opts).blob);
  zip.file(`${base}.txt`, buildTxt(article, opts).blob);
  zip.file(`${base}.md`, buildMarkdown(article, opts).blob);
  zip.file('article.json', `${JSON.stringify(article, null, 2)}\n`);
  const bytes = await zip.generateAsync({
    type: 'uint8array',
    compression: 'DEFLATE',
    compressionOptions: { level: 9 },
    mimeType: 'application/zip',
  });
  return {
    name: `${base}.zip`,
    mime: 'application/zip',
    blob: bytesToBlob(bytes, 'application/zip'),
    format: 'zip',
    estimatedPages: epub.pages,
  };
}
