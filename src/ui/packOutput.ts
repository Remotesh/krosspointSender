/**
 * Pack output. Build the pack EPUB, rebuild it after a title or author
 * change, set the filename, make the pack ZIP, save the pack to the
 * library and log sends. The right panel calls these in pack mode.
 */
import JSZip from 'jszip';
import type { AppSettings, DeviceProfile, ExtractOptions, OutputFile, Pack, PackProgress } from '../types';
import { buildPackEpub } from '../epub';
import { buildHtml, estimateReading, fetchImages, sanitizeFilename } from '../converters';
import { savePack } from '../storage';
import { buildOptions, libraryChanged } from './actions';
import { errorMessage } from './dom';
import { store, type PackOutput } from './state';
import { toast } from './toast';

const EPUB_MIME = 'application/epub+zip';

/** Extract options for pack pages. The pack limits set infobox and references. */
export function packExtractOptions(s: AppSettings, pack: Pack): ExtractOptions {
  return {
    ...s.extract,
    keepLinks: true,
    keepImages: s.build.includeImages,
    keepTables: s.extract.keepTables ?? s.style.tableHandling !== 'drop',
    keepInfobox: pack.limits.includeInfobox,
    keepReferences: pack.limits.includeReferences,
  };
}

/** Fetch the images of each fetched page. Best effort. Returns one map for the pack. */
export async function fetchPackImages(
  pack: Pack,
  onPage: (done: number, total: number, title: string) => void,
  signal?: AbortSignal,
): Promise<Map<string, Blob>> {
  const out = new Map<string, Blob>();
  const pages = pack.pages.filter((p) => p.state === 'done' && p.article && p.article.images.length);
  for (let i = 0; i < pages.length; i++) {
    if (signal?.aborted) throw new DOMException('Cancelled', 'AbortError');
    const a = pages[i].article!;
    onPage(i, pages.length, a.title);
    const max = Math.max(0, pack.limits.maxImagesPerArticle);
    const m = await fetchImages({ ...a, images: a.images.slice(0, max) }, signal);
    for (const [k, v] of m) out.set(k, v);
  }
  onPage(pages.length, pages.length, '');
  return out;
}

/** Replace the dc:creator of an EPUB. Keeps "mimetype" first and stored. */
export async function patchCreator(file: OutputFile, author: string): Promise<OutputFile> {
  const zip = await JSZip.loadAsync(file.blob);
  const opfPath = Object.keys(zip.files).find((n) => n.endsWith('.opf'));
  if (!opfPath) return file;
  const opf = await zip.file(opfPath)!.async('string');
  const esc = author.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const next = /<dc:creator\b[^>]*>[\s\S]*?<\/dc:creator>/.test(opf)
    ? opf.replace(/(<dc:creator\b[^>]*>)[\s\S]*?(<\/dc:creator>)/, `$1${esc}$2`)
    : opf.replace(/(<dc:title\b[^>]*>[\s\S]*?<\/dc:title>)/, `$1\n    <dc:creator id="creator">${esc}</dc:creator>`);
  zip.file(opfPath, next);
  zip.file('mimetype', EPUB_MIME, { compression: 'STORE' });
  const blob = await zip.generateAsync({ type: 'blob', mimeType: EPUB_MIME, compression: 'DEFLATE' });
  return { ...file, blob };
}

/** Build the pack EPUB with the current settings. Applies the author. */
export async function buildPackFile(
  pack: Pack,
  images: Map<string, Blob>,
  author: string,
  onProgress?: (p: PackProgress) => void,
): Promise<OutputFile> {
  const settings = store().get().settings;
  const imgs = settings.build.includeImages ? images : new Map<string, Blob>();
  let file = await buildPackEpub(pack, buildOptions(settings), imgs, onProgress);
  if (author.trim()) file = await patchCreator(file, author.trim());
  return file;
}

/** Put a built pack in the store and save it to the library. */
export async function setBuiltPack(pack: Pack, file: OutputFile, images: Map<string, Blob>): Promise<void> {
  const st = store();
  const out: PackOutput = { file, images, builtTitle: pack.title, builtAuthor: st.get().packAuthor ?? '' };
  st.set({ pack, packOutput: out, mode: 'pack', packFileName: st.get().packFileName || file.name });
  await savePackToLibrary(pack, file.name, images);
}

/** Save the pack. Cache image bytes when the settings allow it. */
export async function savePackToLibrary(
  pack: Pack,
  fileName?: string,
  images?: Map<string, Blob>,
  send?: { ok: boolean; deviceName?: string; method: string; error?: string },
): Promise<void> {
  try {
    const cache = store().get().settings.cacheImages && images && images.size ? Object.fromEntries(images) : undefined;
    await savePack(pack, { fileName, imageBlobs: cache, send });
    libraryChanged();
  } catch (err) {
    toast(`Could not save the pack to the library: ${errorMessage(err)}`, 'warn');
  }
}

/** Filename for the pack: the user's name, made safe, with .epub. */
function finalName(name: string | undefined, fallback: string): string {
  const base = (name || fallback).replace(/\.epub$/i, '');
  return `${sanitizeFilename(base) || 'pack'}.epub`;
}

/**
 * The pack EPUB that the right panel exports or sends.
 * It rebuilds when the title or the author changed after the build.
 */
export async function currentPackFile(): Promise<OutputFile | undefined> {
  const st = store();
  const { pack, packOutput, packAuthor = '', packFileName } = st.get();
  if (!pack || !packOutput) {
    toast('Build the pack first (pack tab, step 4).', 'warn');
    return undefined;
  }
  let out = packOutput;
  if (pack.title !== out.builtTitle || packAuthor !== out.builtAuthor) {
    const file = await buildPackFile(pack, out.images, packAuthor);
    out = { ...out, file, builtTitle: pack.title, builtAuthor: packAuthor };
    st.set({ packOutput: out });
  }
  const file = { ...out.file, name: finalName(packFileName, out.file.name) };
  await savePackToLibrary(pack, file.name);
  return file;
}

/** A ZIP with the pack EPUB and one HTML file per article. */
export async function packZip(epub: OutputFile, pack: Pack): Promise<OutputFile> {
  const zip = new JSZip();
  zip.file(epub.name, epub.blob);
  const opts = buildOptions(store().get().settings);
  const used = new Set<string>();
  for (const p of pack.pages) {
    if (p.state !== 'done' || !p.article) continue;
    const html = buildHtml(p.article, opts);
    let name = `html/${html.name}`;
    for (let n = 2; used.has(name); n++) name = `html/${n}-${html.name}`;
    used.add(name);
    zip.file(name, html.blob);
  }
  const blob = await zip.generateAsync({ type: 'blob', mimeType: 'application/zip', compression: 'DEFLATE' });
  return { name: epub.name.replace(/\.epub$/i, '.zip'), mime: 'application/zip', blob, format: 'zip' };
}

/** Log a send of the current pack in the library. */
export async function recordPackSend(profile: DeviceProfile, ok: boolean, err?: unknown): Promise<void> {
  const { pack, packFileName } = store().get();
  if (!pack) return;
  await savePackToLibrary(pack, packFileName, undefined, {
    ok,
    method: profile.method,
    deviceName: profile.name,
    error: ok ? undefined : errorMessage(err),
  });
}

/** Sum of words, pages and images over the fetched pages. */
export function packStats(pack: Pack): { articles: number; words: number; pages: number; images: number; failed: number } {
  const style = store().get().settings.style;
  let articles = 0;
  let words = 0;
  let pages = 0;
  let images = 0;
  let failed = 0;
  for (const p of pack.pages) {
    if (p.state === 'failed') failed++;
    if (p.state !== 'done' || !p.article) continue;
    articles++;
    words += p.article.wordCount;
    pages += estimateReading(p.article, style).pages;
    images += Math.min(p.article.images.length, pack.limits.maxImagesPerArticle);
  }
  return { articles, words, pages, images, failed };
}
