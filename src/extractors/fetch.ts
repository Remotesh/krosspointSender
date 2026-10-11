/**
 * Fetch an article page from the browser.
 * The fetch sends no cookies. Most sites block it with CORS.
 * The error code tells the UI which fallback to offer.
 */
import { isPrivateHost } from './url';

export type FetchErrorCode = 'cors' | 'network' | 'http' | 'mixed-content' | 'private-network' | 'timeout';

export class FetchError extends Error {
  constructor(
    public code: FetchErrorCode,
    message: string,
    public status?: number,
    public cause?: unknown,
  ) {
    super(message);
    this.name = 'FetchError';
  }
}

/** Fetch time limit in milliseconds. */
export const FETCH_TIMEOUT_MS = 20_000;

interface PageLoc {
  protocol: string;
  origin: string;
  hostname: string;
}

function pageLocation(): PageLoc | null {
  try {
    if (typeof location === 'undefined') return null;
    return { protocol: location.protocol, origin: location.origin, hostname: location.hostname };
  } catch {
    return null;
  }
}

function isLoopback(host: string): boolean {
  const h = host.toLowerCase().replace(/^\[|\]$/g, '');
  return h === 'localhost' || h.endsWith('.localhost') || h === '::1' || /^127\./.test(h);
}

/**
 * Fetch the HTML of a page.
 * Throws FetchError on failure.
 * When the caller aborts through `signal`, the original AbortError goes to the caller.
 */
export async function fetchPage(url: string, signal?: AbortSignal): Promise<{ html: string; finalUrl: string }> {
  let target: URL;
  try {
    target = new URL(url);
  } catch {
    throw new FetchError('network', `This is not a valid URL: ${url}`);
  }
  const page = pageLocation();
  if (page && page.protocol === 'https:' && target.protocol === 'http:' && !isLoopback(target.hostname)) {
    throw new FetchError('mixed-content', 'This app runs on https. The browser blocks requests to http pages (mixed content).');
  }

  const controller = new AbortController();
  let timedOut = false;
  const timer = setTimeout(() => {
    timedOut = true;
    controller.abort();
  }, FETCH_TIMEOUT_MS);
  const onAbort = () => controller.abort();
  if (signal) {
    if (signal.aborted) controller.abort();
    else signal.addEventListener('abort', onAbort, { once: true });
  }

  try {
    let res: Response;
    try {
      res = await fetch(target.toString(), {
        method: 'GET',
        credentials: 'omit',
        redirect: 'follow',
        headers: { Accept: 'text/html,application/xhtml+xml;q=0.9,*/*;q=0.5' },
        signal: controller.signal,
      });
    } catch (err) {
      throw classify(err, target, page, timedOut, signal);
    }
    if (!res.ok) {
      throw new FetchError(
        'http',
        `The site answered with HTTP ${res.status}${res.statusText ? ' ' + res.statusText : ''}.`,
        res.status,
      );
    }
    let html: string;
    try {
      html = await res.text();
    } catch (err) {
      throw classify(err, target, page, timedOut, signal);
    }
    return { html, finalUrl: res.url || target.toString() };
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', onAbort);
  }
}

function classify(err: unknown, target: URL, page: PageLoc | null, timedOut: boolean, userSignal?: AbortSignal): unknown {
  if (err instanceof FetchError) return err;
  const name = (err as { name?: string } | null)?.name;
  if (name === 'AbortError' || name === 'TimeoutError') {
    if (timedOut) {
      return new FetchError('timeout', `The site did not answer in ${FETCH_TIMEOUT_MS / 1000} seconds.`, undefined, err);
    }
    if (userSignal?.aborted) return err; // The user cancelled. Pass the AbortError on.
    return new FetchError('timeout', 'The request stopped before the site answered.', undefined, err);
  }
  if (err instanceof TypeError || name === 'TypeError') {
    if (isPrivateHost(target.hostname) && !(page && isPrivateHost(page.hostname))) {
      return new FetchError(
        'private-network',
        'The browser blocked a request from a public page to a private network address.',
        undefined,
        err,
      );
    }
    if (page && page.origin !== target.origin) {
      return new FetchError('cors', 'The site does not let other web pages read its content (CORS).', undefined, err);
    }
    return new FetchError('network', 'The network request failed. Check the address and your connection.', undefined, err);
  }
  return new FetchError('network', err instanceof Error ? err.message : String(err), undefined, err);
}

const GENERAL_FALLBACKS = [
  'Use the bookmarklet: open the article in a normal tab, then click the K-rosspointSender bookmark.',
  'Open the page, view the source (Ctrl+U), copy all of it, and use "Paste HTML".',
  'Save the page (Ctrl+S, "Webpage, HTML only"), then drop the .html file here.',
  'Select the article text, copy it, and use "Paste text".',
];

/**
 * Plain-word help for a fetch failure.
 * The first lines say what went wrong. The last lines are the fallbacks.
 */
export function fetchErrorHelp(err: unknown): string[] {
  const fe = err instanceof FetchError ? err : null;
  const lines: string[] = [];
  switch (fe?.code ?? 'network') {
    case 'cors':
      lines.push('The site does not let other pages read it (CORS). This is normal. Most sites block it.');
      break;
    case 'network':
      lines.push('The request failed. The address can be wrong, you can be offline, or the site blocked the request.');
      break;
    case 'http': {
      const st = fe?.status;
      let line = `The site answered with an error (HTTP ${st ?? '?'}).`;
      if (st === 401 || st === 403) line += ' The page can need a login, or it blocks automatic reading.';
      else if (st === 404 || st === 410) line += ' Check the address.';
      else if (st === 429) line += ' You sent too many requests. Wait, then try again.';
      lines.push(line);
      break;
    }
    case 'mixed-content':
      lines.push('This app runs on https. The browser does not let it read http pages.');
      lines.push('Try the https version of the address, or run the app locally from http://localhost.');
      break;
    case 'private-network':
      lines.push('The browser blocked a request from a public site to a private network address.');
      lines.push('Run the app locally from http://localhost, or use the local helper app.');
      break;
    case 'timeout':
      lines.push('The site did not answer in time. Try again, or use one of the fallbacks.');
      break;
  }
  return [...lines, ...GENERAL_FALLBACKS];
}

/**
 * Parse a Wikipedia URL into its language code and article title.
 * Returns null when the URL is not a Wikipedia article URL.
 */
export function parseWikipediaUrl(url: string): { lang: string; title: string; host: string } | null {
  let u: URL;
  try {
    u = new URL(url);
  } catch {
    return null;
  }
  const m = /^(?:([a-z-]+)\.)?(?:m\.)?wikipedia\.org$/i.exec(u.hostname);
  if (!m) return null;
  const lang = (m[1] && m[1].toLowerCase() !== 'm' ? m[1] : 'en').toLowerCase();
  let title = '';
  const wiki = /^\/wiki\/(.+)$/.exec(u.pathname);
  if (wiki) title = wiki[1];
  else if (u.searchParams.get('title')) title = u.searchParams.get('title') ?? '';
  if (!title) return null;
  try {
    title = decodeURIComponent(title);
  } catch {
    /* keep the raw title */
  }
  title = title.replace(/_/g, ' ').split('#')[0];
  return { lang, title, host: `${lang}.wikipedia.org` };
}

interface ParseApiResponse {
  error?: { code?: string; info?: string };
  parse?: { title?: string; pageid?: number; text?: string; displaytitle?: string };
}

function escapeHtml(s: string): string {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

/** HTTP statuses that mean "slow down": the request is sent again after a wait. */
const RETRY_STATUSES = new Set([429, 503]);

/** Max wait for one retry, in seconds. A larger Retry-After value is cut to this. */
const MAX_RETRY_WAIT_S = 60;

export interface RetryOptions {
  /** The caller's signal. An abort stops the request and the wait (AbortError). */
  signal?: AbortSignal;
  /** Max retries after the first request (default 3). */
  maxRetries?: number;
  /** Time limit for each request in milliseconds (default FETCH_TIMEOUT_MS). */
  timeoutMs?: number;
  /** Called before each wait with the wait in seconds and the HTTP status. */
  onRetry?: (waitSeconds: number, status: number, attempt: number) => void;
}

/** Read a Retry-After header (seconds or an HTTP date). Returns seconds or null. */
export function parseRetryAfter(value: string | null, now = Date.now()): number | null {
  if (!value) return null;
  const v = value.trim();
  if (/^\d+(\.\d+)?$/.test(v)) return Math.min(MAX_RETRY_WAIT_S, Number(v));
  const t = Date.parse(v);
  if (Number.isNaN(t)) return null;
  return Math.min(MAX_RETRY_WAIT_S, Math.max(0, Math.ceil((t - now) / 1000)));
}

function abortableWait(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    if (signal?.aborted) {
      reject(new DOMException('The operation was cancelled.', 'AbortError'));
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(new DOMException('The operation was cancelled.', 'AbortError'));
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, ms);
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/**
 * GET with a retry on HTTP 429 and 503.
 * The wait is the Retry-After value when the server sends one, else 2 s, 4 s, 8 s.
 * After the last retry the function returns the 429/503 response.
 * Network errors are FetchError; a caller abort is an AbortError.
 */
export async function fetchWithRetry(url: string, init: RequestInit = {}, opts: RetryOptions = {}): Promise<Response> {
  const { signal } = opts;
  const maxRetries = opts.maxRetries ?? 3;
  const timeoutMs = opts.timeoutMs ?? FETCH_TIMEOUT_MS;
  const target = new URL(url);
  for (let attempt = 0; ; attempt++) {
    const controller = new AbortController();
    let timedOut = false;
    const timer = setTimeout(() => {
      timedOut = true;
      controller.abort();
    }, timeoutMs);
    const onAbort = () => controller.abort();
    if (signal) {
      if (signal.aborted) controller.abort();
      else signal.addEventListener('abort', onAbort, { once: true });
    }
    let res: Response;
    try {
      res = await fetch(target.toString(), { ...init, signal: controller.signal });
    } catch (err) {
      throw classify(err, target, pageLocation(), timedOut, signal);
    } finally {
      clearTimeout(timer);
      signal?.removeEventListener('abort', onAbort);
    }
    if (!RETRY_STATUSES.has(res.status) || attempt >= maxRetries) return res;
    const wait = parseRetryAfter(res.headers.get('Retry-After')) ?? 2 ** (attempt + 1);
    opts.onRetry?.(wait, res.status, attempt + 1);
    await abortableWait(wait * 1000, signal);
  }
}

/**
 * Fetch a Wikipedia article through the MediaWiki API.
 * Plain article pages send no CORS headers, but the API does (origin=*).
 * The result is a small HTML document that `extractWikipedia` understands:
 * `#firstHeading` plus `#mw-content-text .mw-parser-output`.
 * On HTTP 429/503 it waits and sends the request again (see fetchWithRetry).
 * `onRetry` gets the wait in seconds.
 */
export async function fetchWikipedia(
  url: string,
  signal?: AbortSignal,
  onRetry?: (waitSeconds: number) => void,
): Promise<{ html: string; finalUrl: string }> {
  const parsed = parseWikipediaUrl(url);
  if (!parsed) throw new FetchError('network', `This is not a Wikipedia article URL: ${url}`);
  const api = new URL(`https://${parsed.host}/w/api.php`);
  api.search = new URLSearchParams({
    action: 'parse',
    page: parsed.title,
    prop: 'text|displaytitle',
    redirects: '1',
    disableeditsection: '1',
    format: 'json',
    formatversion: '2',
    origin: '*',
  }).toString();

  const res = await fetchWithRetry(
    api.toString(),
    { method: 'GET', credentials: 'omit' },
    { signal, onRetry: onRetry ? (w) => onRetry(w) : undefined },
  );
  if (!res.ok) throw new FetchError('http', `Wikipedia answered with HTTP ${res.status}.`, res.status);
  const data = (await res.json()) as ParseApiResponse;
  if (data.error || !data.parse?.text) {
    const info = data.error?.info || 'No article text in the API response.';
    throw new FetchError('http', `Wikipedia could not find this article: ${info}`, 404);
  }
  const title = data.parse.title || parsed.title;
  const finalUrl = `https://${parsed.host}/wiki/${encodeURIComponent(title.replace(/ /g, '_'))}`;
  const html =
    `<!DOCTYPE html><html lang="${escapeHtml(parsed.lang)}"><head><meta charset="utf-8"><title>${escapeHtml(title)} - Wikipedia</title></head>` +
    `<body><h1 id="firstHeading">${escapeHtml(title)}</h1><div id="mw-content-text" lang="${escapeHtml(parsed.lang)}">${data.parse.text}</div></body></html>`;
  return { html, finalUrl };
}
