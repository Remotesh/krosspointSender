/**
 * extractArticle: one entry point for every input kind.
 * The extractor never fetches. For 'url' and 'wikipedia' the content is fetched HTML.
 */
import { Readability } from '@mozilla/readability';
import type { Article, ExtractOptions, RawInput } from '../types';
import { parseBookmarkletPayload } from './bookmarklet';
import { fixImages, normalizeFigures, normText, parseDocument, parseFragment, removeJunk } from './dom';
import { finishArticle } from './finish';
import { readJsonLd, readMeta, toIsoDate } from './meta';
import { sanitizeHtml } from './sanitize';
import { markdownToHtml, textToHtml } from './text';
import { isWikipediaUrl } from './url';
import { extractWikipedia } from './wikipedia';

function sanitizeOpts(opts: ExtractOptions, baseUrl?: string) {
  return {
    baseUrl,
    keepLinks: opts.keepLinks ?? true,
    keepImages: opts.keepImages ?? true,
    keepTables: opts.keepTables ?? true,
  };
}

function fileBaseName(fileName?: string): string | undefined {
  if (!fileName) return undefined;
  const base = fileName.split(/[\\/]/).pop() ?? fileName;
  return normText(base.replace(/\.[a-z0-9]{1,8}$/i, '').replace(/[_]+/g, ' ')) || undefined;
}

function firstHeading(html: string): string | undefined {
  const h = parseFragment(html).body.querySelector('h1, h2, h3, h4, h5, h6');
  return normText(h?.textContent) || undefined;
}

function firstLine(text: string): string | undefined {
  const line = (text ?? '').split(/\r?\n/).map((l) => l.replace(/^#+\s*/, '').trim()).find(Boolean);
  if (!line) return undefined;
  return line.length > 120 ? line.slice(0, 117).trimEnd() + '...' : line;
}

function hostName(url?: string): string | undefined {
  if (!url) return undefined;
  try {
    return new URL(url).hostname.replace(/^www\./, '');
  } catch {
    return undefined;
  }
}

interface HtmlCtx {
  html: string;
  sourceUrl?: string;
  kind: Article['inputKind'];
  capturedAt?: string;
  fileName?: string;
  titleHint?: string;
}

function extractFromHtml(ctx: HtmlCtx, opts: ExtractOptions): Article {
  const doc = parseDocument(ctx.html, ctx.sourceUrl);
  // Metadata first. The steps below change the document.
  const meta = readMeta(doc);
  const ld = readJsonLd(doc);
  const htmlTitle = normText(doc.title) || undefined;

  removeJunk(doc, { patterns: true });
  fixImages(doc.body);
  normalizeFigures(doc.body);

  const bodyText = normText(doc.body.textContent).length;
  let content: string | undefined;
  let reader: ReturnType<Readability<string>['parse']> = null;
  try {
    const clone = doc.cloneNode(true) as Document;
    reader = new Readability(clone, { charThreshold: 250, keepClasses: false }).parse();
  } catch {
    reader = null;
  }
  const readerText = normText(reader?.textContent).length;
  if (reader?.content && readerText >= Math.min(200, bodyText * 0.5)) content = reader.content;
  else content = doc.body.innerHTML;

  const bodyHtml = sanitizeHtml(content, sanitizeOpts(opts, ctx.sourceUrl));
  const byline = normText(reader?.byline).replace(/^by\s+/i, '') || undefined;
  const title =
    ld.headline ||
    normText(reader?.title) ||
    ctx.titleHint ||
    meta.title ||
    htmlTitle ||
    firstHeading(bodyHtml) ||
    fileBaseName(ctx.fileName) ||
    'Untitled';

  return finishArticle({
    bodyHtml,
    inputKind: ctx.kind,
    title,
    author: meta.author ?? (byline && byline.length < 120 ? byline : undefined),
    publishedAt: meta.publishedAt ?? toIsoDate(reader?.publishedTime),
    capturedAt: ctx.capturedAt,
    sourceUrl: ctx.sourceUrl,
    siteName: meta.siteName ?? (normText(reader?.siteName) || undefined) ?? hostName(ctx.sourceUrl),
    excerpt: meta.excerpt ?? (normText(reader?.excerpt) || undefined),
    language: meta.language ?? (normText(reader?.lang) || undefined) ?? 'en',
    tags: meta.tags,
  });
}

/**
 * Turn any raw input into a clean Article.
 * Dispatches on `input.kind`.
 */
export async function extractArticle(input: RawInput, opts: ExtractOptions = {}): Promise<Article> {
  const content = input.content ?? '';
  switch (input.kind) {
    case 'wikipedia':
      if (!content.trim()) throw new Error('No HTML to extract. Fetch the page first.');
      return extractWikipedia(content, input.sourceUrl ?? '', opts);

    case 'markdown': {
      const html = sanitizeHtml(markdownToHtml(content), sanitizeOpts(opts, input.sourceUrl));
      return finishArticle({
        bodyHtml: html,
        inputKind: 'markdown',
        title: firstHeading(html) || firstLine(content) || fileBaseName(input.fileName) || 'Untitled',
        sourceUrl: input.sourceUrl,
      });
    }

    case 'text': {
      const html = textToHtml(content);
      return finishArticle({
        bodyHtml: html,
        inputKind: 'text',
        title: firstLine(content) || fileBaseName(input.fileName) || 'Untitled',
        sourceUrl: input.sourceUrl,
      });
    }

    case 'bookmarklet': {
      let html = content;
      let url = input.sourceUrl;
      let capturedAt: string | undefined;
      let titleHint: string | undefined;
      if (!content.trim().startsWith('<')) {
        const payload = parseBookmarkletPayload(content);
        html = payload.html;
        url = payload.url || url;
        capturedAt = payload.capturedAt;
        titleHint = payload.title;
      }
      if (url && isWikipediaUrl(url)) {
        const a = extractWikipedia(html, url, opts);
        a.inputKind = 'bookmarklet';
        if (capturedAt) a.capturedAt = capturedAt;
        return a;
      }
      return extractFromHtml({ html, sourceUrl: url || undefined, kind: 'bookmarklet', capturedAt, titleHint, fileName: input.fileName }, opts);
    }

    case 'url':
      if (!content.trim()) throw new Error('No HTML to extract. Fetch the page first.');
      if (input.sourceUrl && isWikipediaUrl(input.sourceUrl)) {
        const a = extractWikipedia(content, input.sourceUrl, opts);
        a.inputKind = 'url';
        return a;
      }
      return extractFromHtml({ html: content, sourceUrl: input.sourceUrl, kind: 'url', fileName: input.fileName }, opts);

    case 'html':
    default:
      if (!content.trim()) throw new Error('The HTML is empty.');
      if (input.sourceUrl && isWikipediaUrl(input.sourceUrl)) {
        // Pasted Wikipedia page source. Use the Wikipedia cleanup path.
        const a = extractWikipedia(content, input.sourceUrl, opts);
        a.inputKind = 'html';
        return a;
      }
      return extractFromHtml({ html: content, sourceUrl: input.sourceUrl, kind: 'html', fileName: input.fileName }, opts);
  }
}
