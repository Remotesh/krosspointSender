/**
 * k-rosspointSender shared types.
 * This file is the contract between modules. Do not change a type here
 * without updating docs/ARCHITECTURE.md and every module that uses it.
 */

// ───────────────────────── Input ─────────────────────────

export type InputKind =
  | 'url'          // user pasted a web URL
  | 'wikipedia'    // detected Wikipedia URL
  | 'html'         // pasted or uploaded raw HTML
  | 'text'         // pasted or uploaded plain text
  | 'markdown'     // pasted or uploaded Markdown
  | 'bookmarklet'; // payload captured by the bookmarklet

export interface RawInput {
  kind: InputKind;
  /** Raw content. For 'url'/'wikipedia' this is the fetched HTML (or empty if not fetched yet). */
  content: string;
  /** Source URL when known. Used for link resolution and the source note. */
  sourceUrl?: string;
  /** Original filename when the input came from a file. */
  fileName?: string;
}

/** Payload format produced by the bookmarklet and the optional extension. */
export interface BookmarkletPayload {
  v: 1;
  url: string;
  title?: string;
  html: string;        // document.documentElement.outerHTML or a selection
  capturedAt: string;  // ISO date
}

// ───────────────────────── Extraction ─────────────────────────

export interface ExtractOptions {
  /** Wikipedia: keep infoboxes (default false). */
  keepInfobox?: boolean;
  /** Wikipedia: keep the References/Notes sections (default false). */
  keepReferences?: boolean;
  /** Keep <a> links in the body (default true). If false, links become plain text. */
  keepLinks?: boolean;
  /** Keep images (default true). */
  keepImages?: boolean;
  /** Keep tables (default true). If false, tables become paragraphs per row. */
  keepTables?: boolean;
}

export interface TocEntry {
  /** Element id inside the cleaned body. */
  id: string;
  /** 1..6 */
  level: number;
  text: string;
}

export interface ArticleImage {
  /** Absolute URL of the original image. */
  src: string;
  alt?: string;
  /** Filename to use inside the EPUB, e.g. "img-003.jpg". Set by the converter. */
  epubName?: string;
}

/** A cleaned, sanitized article. The body is safe HTML (allowlist) and linear. */
export interface Article {
  id: string;                 // uuid
  title: string;
  author?: string;
  publishedAt?: string;       // ISO date when found
  capturedAt: string;         // ISO date
  sourceUrl?: string;
  siteName?: string;
  excerpt?: string;
  language?: string;          // BCP47, default "en"
  tags: string[];
  /** Sanitized HTML body (no <html>/<body> wrapper). Headings carry ids for the TOC. */
  bodyHtml: string;
  toc: TocEntry[];
  images: ArticleImage[];
  wordCount: number;
  inputKind: InputKind;
}

// ───────────────────────── Output ─────────────────────────

export type OutputFormat = 'epub' | 'html' | 'txt' | 'md' | 'zip';

export type StylePreset = 'compact' | 'comfortable' | 'large' | 'reference';

export interface StyleSettings {
  preset: StylePreset;
  fontSizePercent: number;   // 100 = 1em
  marginEm: number;          // page/body margin
  lineHeight: number;        // 1.2 .. 2.0
  paragraphSpacingEm: number;
  imageMaxWidthPx: number;   // 480 for the X4 Pro
  grayscaleImages: boolean;
  keepLinks: boolean;
  hyphenate: boolean;
  /** 'keep' | 'linearize' (one paragraph per row) | 'drop' */
  tableHandling: 'keep' | 'linearize' | 'drop';
  /** JPEG quality 0..1 for re-encoded images. */
  imageQuality: number;
  /** Extra CSS appended to the EPUB stylesheet. */
  customCss: string;
}

export interface FilenameRules {
  /** Template, e.g. "{title} - {author}" or "{date} {title}". */
  template: string;
  maxLength: number;      // default 80
  lowercase: boolean;
  spaceReplacement: string; // '' keeps spaces, '-' or '_' replaces them
}

export interface BuildOptions {
  style: StyleSettings;
  includeImages: boolean;
  /** Include the generated cover/title page. */
  includeCover: boolean;
  /** Include the source note at the end. */
  includeSourceNote: boolean;
}

/** One generated output file. */
export interface OutputFile {
  name: string;        // final filename with extension
  mime: string;
  blob: Blob;
  format: OutputFormat;
  /** Approximate page count on the X4 Pro at the chosen style. */
  estimatedPages?: number;
}

// ───────────────────────── Devices and transfer ─────────────────────────

export type FirmwareType = 'crosspoint' | 'kreader' | 'other';

export type TransferMethod =
  | 'webdav'          // WebDAV PUT (CrossPoint port 80, overwrites)
  | 'crosspoint-http' // CrossPoint POST /upload?path= (multipart; rejects duplicates)
  | 'crosspoint-ws'   // CrossPoint WebSocket upload on port 81 with progress
  | 'usb'             // File System Access API folder write
  | 'download'        // manual browser download
  | 'opds'            // generate a local OPDS catalog package
  | 'ble';            // future K-Reader BLE (mock for now)

export interface DeviceProfile {
  id: string;                 // uuid
  name: string;
  firmware: FirmwareType;
  host: string;               // IP or hostname, e.g. "192.168.1.50" or "crosspoint.local"
  port: number;               // 80 default
  method: TransferMethod;
  targetFolder: string;       // e.g. "/Books/K-rosspoint/"
  username?: string;
  password?: string;
  defaultFormat: OutputFormat;
  lastConnectedAt?: string;   // ISO date
  notes?: string;
  createdAt: string;
  updatedAt: string;
}

export type TransferState =
  | 'idle'
  | 'connecting'
  | 'uploading'
  | 'verifying'
  | 'complete'
  | 'failed';

export interface TransferProgress {
  state: TransferState;
  /** 0..1 when known. */
  fraction?: number;
  bytesSent?: number;
  bytesTotal?: number;
  message?: string;
}

/** Why a transfer failed, so the UI can offer the right fallback. */
export type TransferErrorCode =
  | 'network'          // fetch failed: CORS, mixed content, private network access, offline
  | 'mixed-content'    // page is https, device is http
  | 'not-found'        // host unreachable or 404
  | 'auth'             // 401/403
  | 'exists'           // file already exists (CrossPoint /upload)
  | 'unsupported'      // browser lacks the API (File System Access, Bluetooth)
  | 'cancelled'        // user cancelled a picker
  | 'device'           // device returned an error
  | 'unknown';

export class TransferError extends Error {
  constructor(public code: TransferErrorCode, message: string, public cause?: unknown) {
    super(message);
    this.name = 'TransferError';
  }
}

export interface TransferResult {
  ok: boolean;
  /** Final path or location on the device, when known. */
  remotePath?: string;
  bytes: number;
  startedAt: string;
  finishedAt: string;
  error?: TransferError;
}

export interface ConnectionTestResult {
  ok: boolean;
  /** Milliseconds for the round trip. */
  latencyMs?: number;
  /** Device-reported info, e.g. CrossPoint /api/status. */
  info?: Record<string, unknown>;
  error?: TransferError;
}

/** Every transport implements this. All methods must reject with a TransferError, never fail silently. */
export interface TransportAdapter {
  readonly method: TransferMethod;
  /** True if this browser can use this transport at all (feature detection). */
  isSupported(): boolean;
  /** Human text explaining why isSupported() is false, or why it may fail from this origin. */
  limitations(): string[];
  testConnection(profile: DeviceProfile): Promise<ConnectionTestResult>;
  send(
    profile: DeviceProfile,
    file: OutputFile,
    onProgress: (p: TransferProgress) => void,
    signal?: AbortSignal,
  ): Promise<TransferResult>;
}

// ───────────────────────── Storage ─────────────────────────

export interface TransferRecord {
  id: string;
  articleId: string;
  deviceId?: string;
  deviceName?: string;
  method: TransferMethod;
  format: OutputFormat;
  fileName: string;
  bytes: number;
  ok: boolean;
  error?: string;
  at: string; // ISO date
}

/** What the library stores per article. */
export interface LibraryEntry {
  article: Article;
  /** The last output filename used. */
  fileName: string;
  lastFormat: OutputFormat;
  /** Cached image bytes keyed by ArticleImage.src, if the user chose to cache images. */
  imageBlobs?: Record<string, Blob>;
  transfers: TransferRecord[];
  savedAt: string;
  updatedAt: string;
}

// ───────────────────────── Settings ─────────────────────────

export interface AppSettings {
  style: StyleSettings;
  filename: FilenameRules;
  build: Omit<BuildOptions, 'style'>;
  extract: ExtractOptions;
  /** id of the default device profile. */
  defaultDeviceId?: string;
  /** Cache image bytes in the library (default true). */
  cacheImages: boolean;
  /** Optional local helper app base URL, e.g. "http://127.0.0.1:8134". */
  helperUrl?: string;
  /** Last used input mode in the UI. */
  lastInputKind?: InputKind;
  /** Default limits for a new Spider pack. */
  packLimits: PackLimits;
}

// ───────────────────────── Spider packs ─────────────────────────

export type PackSourceKind = 'wikipedia' | 'web';

/** One candidate page found by a scan. */
export interface PackCandidate {
  /** Stable key: Wikipedia canonical title, or absolute URL for web. */
  key: string;
  title: string;
  url: string;
  /** 0 = root, 1 = linked from root, 2 = linked from depth 1. */
  depth: number;
  /** Key of the page that linked to it. */
  parentKey?: string;
  /** How many scanned pages link to it. Used for ordering. */
  linkCount: number;
  selected: boolean;
  /** Set when the scan learned something about the page. */
  flags?: ('disambiguation' | 'redirect' | 'manual')[];
}

export interface PackLimits {
  depth: 0 | 1 | 2;
  maxArticles: number;         // default 20
  maxImagesPerArticle: number; // default 2
  maxTotalEpubSizeMB: number;  // default 25
  requestDelayMs: number;      // default 500
  includeReferences: boolean;  // default false
  includeInfobox: boolean;     // default false
  sameDomainOnly: boolean;     // web spider, default true
}

export type PackPageState = 'queued' | 'fetching' | 'done' | 'failed' | 'skipped';

export interface PackPage {
  candidate: PackCandidate;
  state: PackPageState;
  article?: Article;
  error?: string;
  /** Filename inside the EPUB, e.g. "steam_engine.xhtml". Set by the builder. */
  fileName?: string;
}

export interface PackProgress {
  phase: 'idle' | 'scanning' | 'fetching' | 'building' | 'complete' | 'failed' | 'cancelled';
  done: number;
  total: number;
  currentTitle?: string;
  message?: string;
  errors: { key: string; title: string; error: string }[];
}

/** A built or in-progress pack. Stored in the library next to articles. */
export interface Pack {
  id: string;
  kind: PackSourceKind;
  title: string;
  rootKey: string;
  rootUrl: string;
  limits: PackLimits;
  pages: PackPage[];
  /** Links that point to pages not in the pack, by target key. */
  externalLinks: { key: string; title: string; url: string; fromKey: string }[];
  createdAt: string;
  updatedAt: string;
}
