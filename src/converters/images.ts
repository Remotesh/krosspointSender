/**
 * Image fetch and re-encode for the EPUB.
 * CrossPoint shows only JPEG and PNG. Progressive JPEG looks bad on it.
 * So we re-encode every image to baseline JPEG through a canvas.
 * We keep PNG only for a PNG with transparency when grayscale is off.
 */
import type { Article } from '../types';
import { blobToBytes, bytesToBlob } from './util';

export type ImageMime = 'image/jpeg' | 'image/png';

export interface ProcessedImage {
  blob: Blob;
  mime: ImageMime;
  width: number;
  height: number;
}

export interface ProcessImageOptions {
  maxWidth: number;
  grayscale: boolean;
  quality: number;
}

type Sniffed =
  | 'image/jpeg'
  | 'image/png'
  | 'image/gif'
  | 'image/webp'
  | 'image/bmp'
  | 'image/svg+xml'
  | 'unknown';

/** Finds the image type from the magic bytes. It uses the Blob type only as a fallback. */
export function sniffImageType(bytes: Uint8Array, declared = ''): Sniffed {
  const b = bytes;
  if (b.length >= 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff) return 'image/jpeg';
  if (b.length >= 8 && b[0] === 0x89 && b[1] === 0x50 && b[2] === 0x4e && b[3] === 0x47) return 'image/png';
  if (b.length >= 4 && b[0] === 0x47 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x38) return 'image/gif';
  if (
    b.length >= 12 &&
    b[0] === 0x52 && b[1] === 0x49 && b[2] === 0x46 && b[3] === 0x46 &&
    b[8] === 0x57 && b[9] === 0x45 && b[10] === 0x42 && b[11] === 0x50
  ) {
    return 'image/webp';
  }
  if (b.length >= 2 && b[0] === 0x42 && b[1] === 0x4d) return 'image/bmp';
  const head = new TextDecoder().decode(b.subarray(0, Math.min(b.length, 1024))).toLowerCase();
  if (head.includes('<svg')) return 'image/svg+xml';
  if (declared.toLowerCase() === 'image/svg+xml') return 'image/svg+xml';
  return 'unknown';
}

/** Reads width and height from a PNG or JPEG header. Returns 0x0 when it cannot. */
export function readImageSize(bytes: Uint8Array, mime: ImageMime): { width: number; height: number } {
  const b = bytes;
  if (mime === 'image/png' && b.length >= 24) {
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    return { width: dv.getUint32(16), height: dv.getUint32(20) };
  }
  if (mime === 'image/jpeg') {
    let i = 2;
    while (i + 9 < b.length) {
      if (b[i] !== 0xff) {
        i++;
        continue;
      }
      const marker = b[i + 1];
      if (marker === 0xff) {
        i++;
        continue;
      }
      if (marker === 0xd8 || marker === 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
        i += 2;
        continue;
      }
      const len = (b[i + 2] << 8) | b[i + 3];
      // SOF0..SOF15, but not DHT (C4), JPG (C8) or DAC (CC).
      if (marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc) {
        return { height: (b[i + 5] << 8) | b[i + 6], width: (b[i + 7] << 8) | b[i + 8] };
      }
      i += 2 + len;
    }
  }
  return { width: 0, height: 0 };
}

let canvasProbe: boolean | undefined;

/**
 * Returns true when this environment can draw and encode on a canvas.
 * jsdom has no canvas: getContext returns null or throws there.
 */
export function canvasAvailable(): boolean {
  if (canvasProbe !== undefined) return canvasProbe;
  canvasProbe = false;
  try {
    if (typeof OffscreenCanvas !== 'undefined' && typeof createImageBitmap === 'function') {
      const c = new OffscreenCanvas(1, 1);
      if (c.getContext('2d')) {
        canvasProbe = true;
        return true;
      }
    }
    if (typeof document === 'undefined' || typeof HTMLCanvasElement === 'undefined') return false;
    // jsdom prints a "not implemented" error for getContext. Do not probe there.
    if (typeof navigator !== 'undefined' && /jsdom/i.test(navigator.userAgent)) return false;
    const c = document.createElement('canvas');
    canvasProbe = !!c.getContext('2d');
  } catch {
    canvasProbe = false;
  }
  return canvasProbe;
}

/** For tests: forget the cached canvas probe result. */
export function resetCanvasProbe(): void {
  canvasProbe = undefined;
}

type AnyCanvas = OffscreenCanvas | HTMLCanvasElement;
type Ctx2D = OffscreenCanvasRenderingContext2D | CanvasRenderingContext2D;
type Drawable = ImageBitmap | HTMLImageElement;

async function decodeImage(
  blob: Blob,
  sniffed: Sniffed,
): Promise<{ img: Drawable; w: number; h: number; close: () => void }> {
  // createImageBitmap does not decode SVG in most browsers. Use <img> for SVG.
  if (typeof createImageBitmap === 'function' && sniffed !== 'image/svg+xml') {
    try {
      const bmp = await createImageBitmap(blob);
      return { img: bmp, w: bmp.width, h: bmp.height, close: () => bmp.close() };
    } catch {
      // Fall through to <img>.
    }
  }
  const typed =
    sniffed === 'image/svg+xml' && blob.type !== 'image/svg+xml' ? new Blob([blob], { type: 'image/svg+xml' }) : blob;
  const url = URL.createObjectURL(typed);
  try {
    const img = new Image();
    img.decoding = 'async';
    await new Promise<void>((resolve, reject) => {
      img.onload = () => resolve();
      img.onerror = () => reject(new Error('image decode failed'));
      img.src = url;
    });
    return { img, w: img.naturalWidth, h: img.naturalHeight, close: () => undefined };
  } finally {
    // The image keeps its decoded data. We can revoke the URL soon.
    setTimeout(() => URL.revokeObjectURL(url), 0);
  }
}

function makeCanvas(w: number, h: number): AnyCanvas {
  if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
  const c = document.createElement('canvas');
  c.width = w;
  c.height = h;
  return c;
}

async function encodeCanvas(c: AnyCanvas, mime: ImageMime, quality: number): Promise<Blob> {
  if (typeof OffscreenCanvas !== 'undefined' && c instanceof OffscreenCanvas) {
    return c.convertToBlob({ type: mime, quality });
  }
  return new Promise((resolve, reject) => {
    (c as HTMLCanvasElement).toBlob(
      (b) => (b ? resolve(b) : reject(new Error('canvas encode failed'))),
      mime,
      quality,
    );
  });
}

/** Turns the pixels to gray with Rec. 601 luma. */
function toGray(data: Uint8ClampedArray): void {
  for (let i = 0; i < data.length; i += 4) {
    const y = Math.round(0.299 * data[i] + 0.587 * data[i + 1] + 0.114 * data[i + 2]);
    data[i] = y;
    data[i + 1] = y;
    data[i + 2] = y;
  }
}

function hasTransparency(data: Uint8ClampedArray): boolean {
  for (let i = 3; i < data.length; i += 4) if (data[i] < 255) return true;
  return false;
}

/** Pass-through when there is no canvas: keep JPEG and PNG, drop the rest. */
function passThrough(blob: Blob, bytes: Uint8Array, sniffed: Sniffed): ProcessedImage | null {
  if (sniffed !== 'image/jpeg' && sniffed !== 'image/png') return null;
  const size = readImageSize(bytes, sniffed);
  const out = blob.type === sniffed ? blob : bytesToBlob(bytes, sniffed);
  return { blob: out, mime: sniffed, ...size };
}

/**
 * Re-encodes one image for the device.
 * - Scales down to maxWidth. It never scales up.
 * - Grayscale with per-pixel luma when opts.grayscale is true.
 * - Output is baseline JPEG. Output is PNG only for a transparent PNG with grayscale off.
 * - WebP, GIF, SVG and BMP become JPEG.
 *
 * Returns null when the image cannot be used. This function never throws.
 * Without a canvas (jsdom, some workers) it passes JPEG and PNG through
 * unchanged and returns null for every other type.
 */
export async function processImage(blob: Blob, opts: ProcessImageOptions): Promise<ProcessedImage | null> {
  let bytes: Uint8Array;
  try {
    bytes = await blobToBytes(blob);
  } catch {
    return null;
  }
  if (bytes.length === 0) return null;
  const sniffed = sniffImageType(bytes, blob.type);
  if (sniffed === 'unknown') return null;
  if (!canvasAvailable()) return passThrough(blob, bytes, sniffed);

  try {
    const decoded = await decodeImage(blob, sniffed);
    try {
      const maxWidth = Math.max(16, Math.floor(opts.maxWidth || 480));
      let srcW = decoded.w;
      let srcH = decoded.h;
      // An SVG without a size: give it a default size.
      if (!srcW || !srcH) {
        srcW = maxWidth;
        srcH = Math.round(maxWidth * 0.75);
      }
      const scale = srcW > maxWidth ? maxWidth / srcW : 1;
      const w = Math.max(1, Math.round(srcW * scale));
      const h = Math.max(1, Math.round(srcH * scale));
      const canvas = makeCanvas(w, h);
      const ctx = canvas.getContext('2d') as Ctx2D | null;
      if (!ctx) return passThrough(blob, bytes, sniffed);
      ctx.imageSmoothingEnabled = true;
      ctx.imageSmoothingQuality = 'high';
      ctx.drawImage(decoded.img, 0, 0, w, h);

      let mime: ImageMime = 'image/jpeg';
      if (sniffed === 'image/png' && !opts.grayscale && hasTransparency(ctx.getImageData(0, 0, w, h).data)) {
        mime = 'image/png';
      }
      if (mime === 'image/jpeg') {
        // JPEG has no alpha. Put white behind transparent pixels.
        ctx.globalCompositeOperation = 'destination-over';
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(0, 0, w, h);
        ctx.globalCompositeOperation = 'source-over';
      }
      if (opts.grayscale) {
        const px = ctx.getImageData(0, 0, w, h);
        toGray(px.data);
        ctx.putImageData(px, 0, 0);
      }
      const q = Number.isFinite(opts.quality) ? Math.min(1, Math.max(0.1, opts.quality)) : 0.85;
      const out = await encodeCanvas(canvas, mime, q);
      return { blob: out, mime, width: w, height: h };
    } finally {
      decoded.close();
    }
  } catch {
    return passThrough(blob, bytes, sniffed);
  }
}

/** Decodes a data: URL to a Blob. Returns null when it is not a valid image data URL. */
export function dataUrlToBlob(url: string): Blob | null {
  const m = /^data:([^,]*?),([\s\S]*)$/.exec(url.trim());
  if (!m) return null;
  const meta = m[1].split(';').map((x) => x.trim());
  const mime = (meta[0] || 'text/plain').toLowerCase();
  if (!mime.startsWith('image/')) return null;
  const isB64 = meta.slice(1).some((p) => p.toLowerCase() === 'base64');
  try {
    let bytes: Uint8Array;
    if (isB64) {
      const bin = atob(m[2].replace(/\s+/g, ''));
      bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    } else {
      bytes = new TextEncoder().encode(decodeURIComponent(m[2]));
    }
    return bytesToBlob(bytes, mime);
  } catch {
    return null;
  }
}

/** All image sources of an article: Article.images first, then each <img> in the body. */
export function imageSources(article: Article): string[] {
  const seen = new Set<string>();
  const out: string[] = [];
  const add = (s?: string | null) => {
    const v = (s ?? '').trim();
    if (v && !seen.has(v)) {
      seen.add(v);
      out.push(v);
    }
  };
  for (const im of article.images ?? []) add(im.src);
  if (article.bodyHtml && /<img/i.test(article.bodyHtml)) {
    const doc = new DOMParser().parseFromString(article.bodyHtml, 'text/html');
    doc.querySelectorAll('img').forEach((img) => add(img.getAttribute('src')));
  }
  return out;
}

const FETCH_CONCURRENCY = 4;

/**
 * Fetches the article images. This is best effort.
 * It uses mode "cors". It skips a failed image. It decodes data: URLs
 * locally. The result maps ArticleImage.src to the Blob. It never throws.
 */
export async function fetchImages(article: Article, signal?: AbortSignal): Promise<Map<string, Blob>> {
  const result = new Map<string, Blob>();
  let srcs: string[];
  try {
    srcs = imageSources(article);
  } catch {
    return result;
  }
  let next = 0;
  const worker = async (): Promise<void> => {
    while (next < srcs.length) {
      if (signal?.aborted) return;
      const src = srcs[next++];
      try {
        if (/^data:/i.test(src)) {
          const b = dataUrlToBlob(src);
          if (b && b.size > 0) result.set(src, b);
          continue;
        }
        if (!/^(https?:|blob:)/i.test(src)) continue;
        if (typeof fetch !== 'function') continue;
        const res = await fetch(src, {
          mode: 'cors',
          credentials: 'omit',
          referrerPolicy: 'no-referrer',
          signal,
        });
        if (!res.ok) continue;
        const blob = await res.blob();
        if (blob.size === 0) continue;
        const type = (res.headers?.get('content-type') ?? blob.type ?? '').toLowerCase();
        // Some servers send a generic type. Keep it. processImage sniffs the bytes.
        if (type && !type.startsWith('image/') && !type.startsWith('application/octet-stream')) continue;
        result.set(src, blob);
      } catch {
        // Skip this image. CORS, network and abort errors all come here.
      }
    }
  };
  const n = Math.min(FETCH_CONCURRENCY, srcs.length);
  try {
    await Promise.all(Array.from({ length: n }, () => worker()));
  } catch {
    // Not expected. The workers catch their errors.
  }
  return result;
}
