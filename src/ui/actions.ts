/**
 * UI actions. These functions join the store with the public module APIs.
 * Panels call them. They do not touch the DOM, except for downloads.
 */
import type {
  AppSettings,
  Article,
  BuildOptions,
  DeviceProfile,
  ExtractOptions,
  LibraryEntry,
  OutputFile,
  OutputFormat,
  RawInput,
  TransferMethod,
  TransferProgress,
  TransferResult,
} from '../types';
import { TransferError } from '../types';
import { extractArticle, fetchPage, fetchWikipedia, isWikipediaUrl, normalizeUrl } from '../extractors';
import { buildHtml, buildMarkdown, buildTxt, buildZip, fetchImages } from '../converters';
import { buildEpub } from '../epub';
import { getTransport } from '../transports';
import { defaultProfile, markConnected } from '../deviceProfiles';
import { addTransfer, getArticle, saveArticle } from '../storage';
import { saveSettings } from '../settings';
import { store } from './state';
import { errorMessage } from './dom';
import { toast } from './toast';

/** Image bytes per article id, so a second export does not fetch again. */
const imageCache = new Map<string, Map<string, Blob>>();

export function buildOptions(s: AppSettings): BuildOptions {
  return { style: s.style, ...s.build };
}

/** Extract options from the settings. Style flags also apply. */
function extractOptions(s: AppSettings): ExtractOptions {
  return {
    ...s.extract,
    keepLinks: s.extract.keepLinks ?? s.style.keepLinks,
    keepTables: s.extract.keepTables ?? s.style.tableHandling !== 'drop',
  };
}

/** Run the extractor on an input and put the article in the store. */
export async function runExtract(input: RawInput): Promise<Article | undefined> {
  const st = store();
  st.set({ busy: true, input });
  try {
    const article = await extractArticle(input, extractOptions(st.get().settings));
    // A new article ends pack mode. The pack stays in the store.
    st.set({ article, busy: false, transfer: { state: 'idle' }, mode: 'article' });
    void rememberKind(input.kind);
    return article;
  } catch (err) {
    st.set({ busy: false });
    toast(`Extraction failed: ${errorMessage(err)}`, 'error');
    return undefined;
  }
}

async function rememberKind(kind: RawInput['kind']): Promise<void> {
  const st = store();
  if (st.get().settings.lastInputKind === kind) return;
  st.updateSettings((s) => ({ ...s, lastInputKind: kind }));
  await persistSettings();
}

/** Fetch a URL, then extract. Throws the FetchError so the caller can explain it. */
export async function fetchAndExtract(raw: string, signal?: AbortSignal): Promise<Article | undefined> {
  const url = normalizeUrl(raw);
  if (!url) throw new Error('This is not a valid http(s) URL.');
  const st = store();
  st.set({ busy: true });
  try {
    // Wikipedia pages send no CORS headers. The MediaWiki API does.
    const { html, finalUrl } = isWikipediaUrl(url) ? await fetchWikipedia(url, signal) : await fetchPage(url, signal);
    st.set({ busy: false });
    const kind = isWikipediaUrl(finalUrl) ? 'wikipedia' : 'url';
    return await runExtract({ kind, content: html, sourceUrl: finalUrl });
  } catch (err) {
    st.set({ busy: false });
    throw err;
  }
}

/** Save the settings. Show a toast on failure. */
export async function persistSettings(): Promise<void> {
  try {
    await saveSettings(store().get().settings);
  } catch (err) {
    toast(`Could not save settings: ${errorMessage(err)}`, 'error');
  }
}

/** Get image bytes for an article: from the cache, or fetch them. */
export async function imagesFor(article: Article, preset?: Map<string, Blob>): Promise<Map<string, Blob>> {
  if (preset && preset.size) {
    imageCache.set(article.id, preset);
    return preset;
  }
  const cached = imageCache.get(article.id);
  if (cached) return cached;
  if (!article.images.length) return new Map();
  const map = await fetchImages(article);
  imageCache.set(article.id, map);
  return map;
}

/** Turn a stored image record into a Map. */
export function blobsToMap(rec?: Record<string, Blob>): Map<string, Blob> | undefined {
  return rec ? new Map(Object.entries(rec)) : undefined;
}

/** Build one output file. Fetches images for EPUB and ZIP when needed. */
export async function buildOutput(
  article: Article,
  format: OutputFormat,
  settings: AppSettings,
  preset?: Map<string, Blob>,
): Promise<{ file: OutputFile; images?: Map<string, Blob> }> {
  const opts = buildOptions(settings);
  const needImages = opts.includeImages && (format === 'epub' || format === 'zip');
  const images = needImages ? await imagesFor(article, preset) : preset;
  switch (format) {
    case 'epub':
      return { file: await buildEpub(article, opts, images), images };
    case 'zip':
      return { file: await buildZip(article, opts, images), images };
    case 'html':
      return { file: buildHtml(article, opts), images };
    case 'txt':
      return { file: buildTxt(article, opts), images };
    case 'md':
      return { file: buildMarkdown(article, opts), images };
  }
}

/** Save the article and its last output to the library. Keeps old transfers. */
export async function saveToLibrary(
  article: Article,
  file: OutputFile,
  images?: Map<string, Blob>,
): Promise<void> {
  const settings = store().get().settings;
  const now = new Date().toISOString();
  try {
    const old = await getArticle(article.id);
    let imageBlobs: Record<string, Blob> | undefined;
    if (settings.cacheImages) {
      imageBlobs = images && images.size ? Object.fromEntries(images) : old?.imageBlobs;
    }
    const entry: LibraryEntry = {
      article,
      fileName: file.name,
      lastFormat: file.format === 'file' ? (old?.lastFormat ?? 'epub') : file.format,
      imageBlobs,
      transfers: old?.transfers ?? [],
      savedAt: old?.savedAt ?? now,
      updatedAt: now,
    };
    await saveArticle(entry);
    libraryChanged();
  } catch (err) {
    toast(`Could not save to library: ${errorMessage(err)}`, 'warn');
  }
}

/** Tell the library drawer to reload. */
export function libraryChanged(): void {
  window.dispatchEvent(new CustomEvent('krosspoint:library-changed'));
}

/** Start a browser download of a file. */
export function downloadFile(file: Pick<OutputFile, 'name' | 'blob'>): void {
  const url = URL.createObjectURL(file.blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = file.name;
  a.rel = 'noopener';
  document.body.appendChild(a);
  a.click();
  a.remove();
  window.setTimeout(() => URL.revokeObjectURL(url), 30_000);
}

/** A profile to use for USB or download. It copies the selected device when there is one. */
export function fallbackProfile(method: TransferMethod): DeviceProfile {
  const base = defaultProfile('crosspoint');
  const sel = store().selectedProfile();
  const now = new Date().toISOString();
  return {
    ...base,
    ...(sel ?? {}),
    id: sel?.id ?? 'adhoc',
    name: sel?.name ?? 'Ad hoc',
    method,
    createdAt: sel?.createdAt ?? now,
    updatedAt: sel?.updatedAt ?? now,
  };
}

/** Make a TransferError from any thrown value. */
export function asTransferError(err: unknown): TransferError {
  if (err instanceof TransferError) return err;
  const code = (err as { code?: unknown } | null)?.code;
  if (err instanceof Error && typeof code === 'string') {
    return new TransferError(code as TransferError['code'], err.message, err);
  }
  return new TransferError('unknown', errorMessage(err), err);
}

/**
 * Send a file with a transport. Report progress to the store.
 * Record the transfer in the library. Return the result or throw a TransferError.
 */
export async function sendFile(
  profile: DeviceProfile,
  file: OutputFile,
  articleId?: string,
  onProgress?: (p: TransferProgress) => void,
): Promise<TransferResult> {
  const st = store();
  const report = (p: TransferProgress): void => {
    st.set({ transfer: p });
    onProgress?.(p);
  };
  report({ state: 'connecting', message: `Connecting (${profile.method})` });
  const startedAt = new Date().toISOString();
  let result: TransferResult;
  try {
    result = await getTransport(profile.method).send(profile, file, report);
    if (!result.ok) throw result.error ?? new TransferError('unknown', 'The transfer did not finish.');
    report({
      state: 'complete',
      fraction: 1,
      bytesSent: result.bytes,
      bytesTotal: result.bytes,
      message: result.remotePath ? `Saved as ${result.remotePath}` : `Sent ${file.name}`,
    });
    if (profile.id !== 'adhoc') void markConnected(profile.id).catch(() => undefined);
  } catch (err) {
    const te = asTransferError(err);
    report({ state: 'failed', message: te.message });
    result = { ok: false, bytes: 0, startedAt, finishedAt: new Date().toISOString(), error: te };
  }
  if (articleId) {
    try {
      await addTransfer(articleId, {
        id: crypto.randomUUID(),
        articleId,
        deviceId: profile.id === 'adhoc' ? undefined : profile.id,
        deviceName: profile.name,
        method: profile.method,
        format: file.format,
        fileName: file.name,
        bytes: result.bytes || file.blob.size,
        ok: result.ok,
        error: result.error?.message,
        at: new Date().toISOString(),
      });
      libraryChanged();
    } catch {
      // The article may not be in the library. That is not an error for the send.
    }
  }
  if (!result.ok) throw result.error ?? new TransferError('unknown', 'The transfer failed.');
  return result;
}
