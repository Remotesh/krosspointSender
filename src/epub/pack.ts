/**
 * Spider pack EPUB builder (EPUB 3 + NCX fallback).
 * Layout:
 *   mimetype                              (first entry, stored)
 *   META-INF/container.xml
 *   EPUB/package.opf
 *   EPUB/nav.xhtml
 *   EPUB/toc.ncx
 *   EPUB/css/krosspoint.css
 *   EPUB/articles/index.xhtml             title page + "Included articles"
 *   EPUB/articles/<slug>.xhtml            one file per bundled page
 *   EPUB/articles/external-links.xhtml    external links, grouped by the holding article
 *   EPUB/articles/missing.xhtml           linked pages that the pack does not hold
 *   EPUB/articles/attribution.xhtml       sources, capture dates, license note
 *   EPUB/images/<slug>_01.jpg ...
 * The builder calls rewriteLinks first when a page has no fileName.
 * It enforces limits.maxTotalEpubSizeMB: it removes the images of the
 * largest pages first. If the EPUB is still too large, it throws an Error.
 */
import JSZip from 'jszip';
import type { Article, BuildOptions, OutputFile, Pack, PackPage, PackProgress } from '../types';
import { processImage } from '../converters/images';
import { appendTitleBlock, copyNodes, defaultHref, prepareBody, xel, type CopyContext } from '../converters/prepare';
import { estimateReading } from '../converters/reading';
import { sanitizeFilename } from '../converters/filename';
import {
  APP_NAME,
  bookIdentifier,
  bytesToBlob,
  collapseWs,
  escapeXml,
  isoDay,
  langOf,
  modifiedStamp,
  pad,
} from '../converters/util';
import { packSlug, rewriteLinks } from '../spider';
import {
  EPUB_MIME,
  buildTree,
  ensureContent,
  headingLabel,
  headingsIn,
  idsIn,
  navXhtml,
  ncxXml,
  newXhtml,
  serializeXhtml,
  type Book,
  type NavItem,
} from './build';
import { epubCss } from './css';

// ───────────────────────── Constants ─────────────────────────

const ROOT = 'EPUB';
const CSS_PATH = 'css/krosspoint.css';
/** The CSS link from a file in EPUB/articles/. */
const CSS_FROM_ARTICLES = `../${CSS_PATH}`;
const INDEX_FILE = 'index.xhtml';
const EXTERNAL_FILE = 'external-links.xhtml';
const MISSING_FILE = 'missing.xhtml';
const ATTRIBUTION_FILE = 'attribution.xhtml';
const RESERVED = new Set([INDEX_FILE, EXTERNAL_FILE, MISSING_FILE, ATTRIBUTION_FILE, 'nav.xhtml']);
const CC_BY_SA_URL = 'https://creativecommons.org/licenses/by-sa/4.0/';
const MB = 1024 * 1024;

/** Extra CSS for the pack pages. */
const PACK_CSS = `
/* Spider pack additions */
.source { font-size: 0.85em; font-style: italic; margin: 0 0 1em 0; text-indent: 0; }
.pack-list li { margin: 0.2em 0; }
.pack-url { font-size: 0.8em; word-wrap: break-word; }
`;

const CONTAINER_XML = `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="${ROOT}/package.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
`;

// ───────────────────────── Types ─────────────────────────

interface PackImage {
  src: string;
  /** Path inside EPUB/, e.g. "images/steam_engine_01.jpg". */
  path: string;
  id: string;
  mime: string;
  blob: Blob;
}

interface PreparedPage {
  page: PackPage;
  article: Article;
  title: string;
  fileName: string;
  slug: string;
  nodes: Node[];
  ids: Set<string>;
  images: PackImage[];
  imageBytes: number;
}

interface TextFile {
  file: string;
  id: string;
  xhtml: string;
}

type ExternalLink = Pack['externalLinks'][number];

// ───────────────────────── Small helpers ─────────────────────────

function isBundled(p: PackPage): boolean {
  return p.state === 'done' && !!p.article;
}

function hostOf(url: string | undefined): string {
  if (!url) return '';
  try {
    return new URL(url).host;
  } catch {
    return '';
  }
}

/** Returns a valid XML id from a slug. */
function xmlId(prefix: string, slug: string): string {
  return `${prefix}-${slug.replace(/[^A-Za-z0-9_.-]+/g, '-')}`;
}

function text(doc: Document, parent: Element, tag: string, value: string, cls?: string): Element {
  const e = xel(doc, tag, cls);
  e.appendChild(doc.createTextNode(collapseWs(value)));
  parent.appendChild(e);
  return e;
}

function link(doc: Document, parent: Element, href: string, label: string): Element {
  const a = xel(doc, 'a');
  a.setAttribute('href', href);
  a.appendChild(doc.createTextNode(collapseWs(label) || href));
  parent.appendChild(a);
  return a;
}

/** Converts a URL to absolute http(s). Returns null for other schemes. */
function absoluteUrl(raw: string, base: string | undefined): string | null {
  if (!raw) return null;
  try {
    const u = base ? new URL(raw, base) : new URL(raw);
    return u.protocol === 'http:' || u.protocol === 'https:' ? u.href : null;
  } catch {
    return null;
  }
}

/** True when the URL points to a page (Wikipedia article or web page), not to a file or a citation. */
function isPageUrl(url: string, kind: Pack['kind']): boolean {
  let u: URL;
  try {
    u = new URL(url);
  } catch {
    return false;
  }
  if (u.protocol !== 'http:' && u.protocol !== 'https:') return false;
  if (kind === 'wikipedia') return /(^|\.)wikipedia\.org$/i.test(u.hostname) && u.pathname.startsWith('/wiki/');
  return !/\.(jpe?g|png|gif|webp|svg|pdf|zip|mp3|mp4)$/i.test(u.pathname);
}

function progress(
  onProgress: ((p: PackProgress) => void) | undefined,
  p: Omit<PackProgress, 'errors'>,
  errors: PackProgress['errors'],
): void {
  if (!onProgress) return;
  try {
    onProgress({ ...p, errors });
  } catch {
    // A failing listener must not stop the build.
  }
}

// ───────────────────────── Page resolution ─────────────────────────

/**
 * Returns the bundled pages with a fileName and the external links.
 * It calls rewriteLinks when a bundled page has no fileName.
 * File names that collide with the generated pages get a "-page" suffix.
 */
function resolvePages(pack: Pack): { pages: PackPage[]; external: ExternalLink[]; renames: Map<string, string> } {
  let pages = pack.pages.filter(isBundled);
  let external: ExternalLink[] = [...(pack.externalLinks ?? [])];
  if (pages.some((p) => !p.fileName)) {
    const r = rewriteLinks(pages, pack.kind);
    pages = r.pages.filter(isBundled);
    external = external.concat(r.external ?? []);
  }
  // Remove duplicate external links (same target from the same page).
  const seen = new Set<string>();
  external = external.filter((e) => {
    const k = `${e.fromKey}\u0000${e.key}`;
    if (seen.has(k)) return false;
    seen.add(k);
    return true;
  });

  // Make sure that each page has a unique, safe fileName.
  const used = new Set<string>(RESERVED);
  const renames = new Map<string, string>();
  pages = pages.map((p) => {
    const original = p.fileName ?? '';
    let base = original.replace(/\.xhtml$/i, '');
    if (!/^[A-Za-z0-9_][A-Za-z0-9_.-]*$/.test(base)) base = packSlug(p.candidate.title || p.article?.title || 'page');
    if (!base) base = 'page';
    let name = `${base}.xhtml`;
    let n = 2;
    if (used.has(name)) {
      name = `${base}-page.xhtml`;
      while (used.has(name)) name = `${base}-page-${n++}.xhtml`;
    }
    used.add(name);
    if (original && original !== name && !renames.has(original)) renames.set(original, name);
    return name === original ? p : { ...p, fileName: name };
  });
  return { pages, external, renames };
}

// ───────────────────────── Images ─────────────────────────

async function collectPageImages(
  nodes: Node[],
  slug: string,
  max: number,
  opts: BuildOptions,
  images: Map<string, Blob> | undefined,
): Promise<PackImage[]> {
  const out: PackImage[] = [];
  if (!opts.includeImages || !images || images.size === 0 || max <= 0) return out;
  const srcs: string[] = [];
  for (const n of nodes) {
    if (n.nodeType !== 1) continue;
    const e = n as Element;
    const list = e.localName.toLowerCase() === 'img' ? [e] : Array.from(e.querySelectorAll('img'));
    for (const img of list) {
      const s = (img.getAttribute('src') ?? '').trim();
      if (s && !srcs.includes(s)) srcs.push(s);
    }
  }
  const style = opts.style;
  for (const src of srcs) {
    if (out.length >= max) break;
    const blob = images.get(src);
    if (!blob) continue;
    const res = await processImage(blob, {
      maxWidth: style.imageMaxWidthPx > 0 ? Math.min(style.imageMaxWidthPx, 2000) : 480,
      grayscale: style.grayscaleImages,
      quality: style.imageQuality,
    });
    if (!res) continue;
    const base = `${slug}_${pad(out.length + 1, 2)}`;
    const ext = res.mime === 'image/png' ? 'png' : 'jpg';
    out.push({ src, path: `images/${base}.${ext}`, id: xmlId('img', base), mime: res.mime, blob: res.blob });
  }
  return out;
}

// ───────────────────────── Article files ─────────────────────────

function articleCopyContext(
  p: PreparedPage,
  imageMap: Map<string, string>,
  fileIds: Map<string, Set<string>>,
  renames: Map<string, string>,
): CopyContext {
  const base = p.article.sourceUrl || p.page.candidate.url;
  return {
    image: (src) => imageMap.get(src) ?? null,
    href: (raw) => {
      if (!raw) return null;
      if (raw.startsWith('#')) {
        const id = raw.slice(1);
        return id && p.ids.has(id) ? raw : null;
      }
      // A link to another bundled page, set by rewriteLinks.
      const m = /^([A-Za-z0-9_][A-Za-z0-9_.%-]*\.xhtml)(?:#(.*))?$/i.exec(raw);
      if (m) {
        const file = renames.get(m[1]) ?? m[1];
        const ids = fileIds.get(file);
        if (!ids) return null;
        const frag = m[2] ?? '';
        if (file === p.fileName) return frag && ids.has(frag) ? `#${frag}` : file;
        return frag && ids.has(frag) ? `${file}#${frag}` : file;
      }
      const keep = defaultHref(raw);
      if (keep !== null) return keep;
      // Protocol-relative or site-relative links become absolute.
      return absoluteUrl(raw, base);
    },
  };
}

function articleXhtml(
  p: PreparedPage,
  lang: string,
  withImages: boolean,
  fileIds: Map<string, Set<string>>,
  renames: Map<string, string>,
): string {
  const { doc, body } = newXhtml(p.title, lang, CSS_FROM_ARTICLES);
  const section = xel(doc, 'section');
  text(doc, section, 'h1', p.title);
  const site = collapseWs(p.article.siteName ?? '') || hostOf(p.article.sourceUrl || p.page.candidate.url) || 'the web';
  const day = isoDay(p.article.capturedAt) || isoDay(new Date().toISOString());
  text(doc, section, 'p', `Source: ${site}, captured ${day}`, 'source');
  const imageMap = new Map<string, string>();
  if (withImages) for (const im of p.images) imageMap.set(im.src, `../${im.path}`);
  copyNodes(p.nodes, section, articleCopyContext(p, imageMap, fileIds, renames));
  body.appendChild(section);
  ensureContent(body);
  return serializeXhtml(doc);
}

// ───────────────────────── Generated pages ─────────────────────────

function indexXhtml(pack: Pack, pages: PreparedPage[], lang: string, title: string, today: string): string {
  const { doc, body } = newXhtml(title, lang, CSS_FROM_ARTICLES);
  const div = xel(doc, 'div', 'cover');
  appendTitleBlock(div, title, [
    { label: 'Source', value: pack.rootUrl },
    { label: 'Articles', value: String(pages.length) },
    { label: 'Created', value: today },
  ]);
  body.appendChild(div);
  text(doc, body, 'h2', 'Included articles');
  if (pages.length === 0) {
    text(doc, body, 'p', 'This pack has no articles.');
  } else {
    const ol = xel(doc, 'ol', 'pack-list');
    for (const p of pages) link(doc, ol.appendChild(xel(doc, 'li')), p.fileName, p.title);
    body.appendChild(ol);
  }
  return serializeXhtml(doc);
}

function externalXhtml(external: ExternalLink[], byKey: Map<string, PreparedPage>, lang: string): string {
  const { doc, body } = newXhtml('External links', lang, CSS_FROM_ARTICLES);
  text(doc, body, 'h1', 'External links');
  const groups = new Map<string, ExternalLink[]>();
  for (const e of external) {
    if (!absoluteUrl(e.url, undefined)) continue;
    const list = groups.get(e.fromKey) ?? [];
    list.push(e);
    groups.set(e.fromKey, list);
  }
  if (groups.size === 0) {
    text(doc, body, 'p', 'The articles in this pack have no external links.');
    return serializeXhtml(doc);
  }
  text(doc, body, 'p', 'These links go to pages outside this book. They need a network connection.');
  for (const [fromKey, list] of groups) {
    const holder = byKey.get(fromKey);
    const h = xel(doc, 'h2');
    if (holder) link(doc, h, holder.fileName, holder.title);
    else h.appendChild(doc.createTextNode(collapseWs(fromKey) || 'Other'));
    body.appendChild(h);
    const ul = xel(doc, 'ul', 'pack-list');
    for (const e of list) link(doc, ul.appendChild(xel(doc, 'li')), e.url, e.title || e.url);
    body.appendChild(ul);
  }
  return serializeXhtml(doc);
}

function missingXhtml(pack: Pack, external: ExternalLink[], bundledKeys: Set<string>, lang: string): string {
  const { doc, body } = newXhtml('Missing linked articles', lang, CSS_FROM_ARTICLES);
  text(doc, body, 'h1', 'Missing linked articles');
  const items = new Map<string, { title: string; url: string; note?: string }>();
  // Pages that the pack tried to fetch but does not hold.
  for (const p of pack.pages) {
    if (isBundled(p) || bundledKeys.has(p.candidate.key)) continue;
    const url = absoluteUrl(p.candidate.url, undefined);
    if (!url) continue;
    const note = p.state === 'failed' ? 'fetch failed' : p.state === 'skipped' ? 'skipped' : undefined;
    items.set(p.candidate.key, { title: p.candidate.title || url, url, note });
  }
  // Linked pages that the pack does not hold.
  for (const e of external) {
    if (bundledKeys.has(e.key) || items.has(e.key)) continue;
    const url = absoluteUrl(e.url, undefined);
    if (!url || !isPageUrl(url, pack.kind)) continue;
    items.set(e.key, { title: e.title || url, url });
  }
  if (items.size === 0) {
    text(doc, body, 'p', 'All linked articles that the scan found are in this book.');
    return serializeXhtml(doc);
  }
  text(doc, body, 'p', 'The articles in this book link to these pages. This book does not hold them.');
  const ul = xel(doc, 'ul', 'pack-list');
  for (const it of items.values()) {
    const li = xel(doc, 'li');
    link(doc, li, it.url, it.title);
    if (it.note) li.appendChild(doc.createTextNode(` (${it.note})`));
    li.appendChild(xel(doc, 'br'));
    text(doc, li, 'span', it.url, 'pack-url');
    ul.appendChild(li);
  }
  body.appendChild(ul);
  return serializeXhtml(doc);
}

function attributionXhtml(pack: Pack, pages: PreparedPage[], lang: string, today: string): string {
  const { doc, body } = newXhtml('Sources and attribution', lang, CSS_FROM_ARTICLES);
  text(doc, body, 'h1', 'Sources and attribution');
  if (pack.kind === 'wikipedia') {
    const p = xel(doc, 'p');
    p.appendChild(doc.createTextNode('The articles come from Wikipedia. Text is available under the '));
    link(doc, p, CC_BY_SA_URL, 'Creative Commons Attribution-ShareAlike 4.0 License');
    p.appendChild(doc.createTextNode('. The authors are the Wikipedia contributors. See the history of each article at its URL.'));
    body.appendChild(p);
  } else {
    text(doc, body, 'p', 'The articles come from the pages below. The rights stay with their authors.');
  }
  const ol = xel(doc, 'ol', 'pack-list');
  for (const pg of pages) {
    const li = xel(doc, 'li');
    const url = absoluteUrl(pg.article.sourceUrl || pg.page.candidate.url, undefined);
    li.appendChild(doc.createTextNode(`${pg.title}`));
    const day = isoDay(pg.article.capturedAt);
    if (day) li.appendChild(doc.createTextNode(`, captured ${day}`));
    if (url) {
      li.appendChild(xel(doc, 'br'));
      link(doc, li, url, url);
    }
    ol.appendChild(li);
  }
  body.appendChild(ol);
  text(doc, body, 'p', `Generated by ${APP_NAME} on ${today}.`);
  return serializeXhtml(doc);
}

// ───────────────────────── OPF ─────────────────────────

function packOpf(book: Book, files: TextFile[], images: PackImage[]): string {
  const meta: string[] = [];
  meta.push(`    <dc:identifier id="bookid">${escapeXml(book.identifier)}</dc:identifier>`);
  meta.push(`    <dc:title>${escapeXml(book.title)}</dc:title>`);
  if (book.creator) meta.push(`    <dc:creator id="creator">${escapeXml(book.creator)}</dc:creator>`);
  meta.push(`    <dc:language>${escapeXml(book.language)}</dc:language>`);
  meta.push(`    <dc:date>${escapeXml(book.date)}</dc:date>`);
  for (const s of book.sources) meta.push(`    <dc:source>${escapeXml(s)}</dc:source>`);
  for (const t of book.subjects) meta.push(`    <dc:subject>${escapeXml(t)}</dc:subject>`);
  meta.push(`    <meta property="dcterms:modified">${modifiedStamp()}</meta>`);
  meta.push(`    <meta name="generator" content="${APP_NAME}"/>`);
  const items = [
    '    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
    '    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
    `    <item id="css" href="${CSS_PATH}" media-type="text/css"/>`,
  ];
  for (const f of files) {
    items.push(`    <item id="${escapeXml(f.id)}" href="articles/${escapeXml(f.file)}" media-type="application/xhtml+xml"/>`);
  }
  for (const im of images) {
    items.push(`    <item id="${escapeXml(im.id)}" href="${escapeXml(im.path)}" media-type="${im.mime}"/>`);
  }
  const spine = files.map((f) => `    <itemref idref="${escapeXml(f.id)}"/>`);
  const guide = [
    `    <reference type="title-page" title="Title page" href="articles/${INDEX_FILE}"/>`,
    '    <reference type="toc" title="Contents" href="nav.xhtml"/>',
  ];
  const first = files.find((f) => f.file !== INDEX_FILE);
  if (first) guide.push(`    <reference type="text" title="Start" href="articles/${escapeXml(first.file)}"/>`);
  return `<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid" xml:lang="${escapeXml(book.language)}">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
${meta.join('\n')}
  </metadata>
  <manifest>
${items.join('\n')}
  </manifest>
  <spine toc="ncx">
${spine.join('\n')}
  </spine>
  <guide>
${guide.join('\n')}
  </guide>
</package>
`;
}

// ───────────────────────── Public API ─────────────────────────

/**
 * Builds one EPUB from a spider pack.
 * - Uses only the pages with state "done" and an article.
 * - `images` holds blobs keyed by ArticleImage.src. Each page keeps at most
 *   limits.maxImagesPerArticle images.
 * - Throws an Error when the EPUB is larger than limits.maxTotalEpubSizeMB
 *   after the builder removed all images.
 */
export async function buildPackEpub(
  pack: Pack,
  opts: BuildOptions,
  images?: Map<string, Blob>,
  onProgress?: (p: PackProgress) => void,
): Promise<OutputFile> {
  const style = opts.style;
  const title = collapseWs(pack.title ?? '') || 'K-rosspoint pack';
  const today = isoDay(new Date().toISOString());
  const errors: PackProgress['errors'] = pack.pages
    .filter((p) => p.state === 'failed')
    .map((p) => ({ key: p.candidate.key, title: p.candidate.title, error: p.error ?? 'fetch failed' }));

  const { pages, external, renames } = resolvePages(pack);
  const firstArticle = pages[0]?.article;
  const lang = firstArticle ? langOf(firstArticle) : 'en';
  const maxImages = Math.max(0, Math.floor(pack.limits?.maxImagesPerArticle ?? 2));

  // Pass 1: prepare each page and process its images.
  const prepared: PreparedPage[] = [];
  let estimatedPages = 0;
  for (let i = 0; i < pages.length; i++) {
    const page = pages[i];
    const article = page.article as Article;
    const pageTitle = collapseWs(article.title || page.candidate.title || '') || `Article ${i + 1}`;
    progress(onProgress, { phase: 'building', done: i, total: pages.length, currentTitle: pageTitle }, errors);
    const fileName = page.fileName as string;
    const slug = fileName.replace(/\.xhtml$/i, '');
    const body = prepareBody(article, style);
    const nodes = Array.from(body.root.childNodes);
    const imgs = await collectPageImages(nodes, slug, maxImages, opts, images);
    prepared.push({
      page,
      article,
      title: pageTitle,
      fileName,
      slug,
      nodes,
      ids: new Set(idsIn(nodes)),
      images: imgs,
      imageBytes: imgs.reduce((s, im) => s + im.blob.size, 0),
    });
    estimatedPages += estimateReading(article, style).pages;
  }

  const fileIds = new Map<string, Set<string>>();
  for (const p of prepared) fileIds.set(p.fileName, p.ids);
  const byKey = new Map<string, PreparedPage>();
  for (const p of prepared) byKey.set(p.page.candidate.key, p);
  const bundledKeys = new Set(byKey.keys());

  // The generated pages do not change between size passes.
  const indexFile: TextFile = { file: INDEX_FILE, id: 'index', xhtml: indexXhtml(pack, prepared, lang, title, today) };
  const tail: TextFile[] = [
    { file: EXTERNAL_FILE, id: 'external-links', xhtml: externalXhtml(external, byKey, lang) },
    { file: MISSING_FILE, id: 'missing', xhtml: missingXhtml(pack, external, bundledKeys, lang) },
    { file: ATTRIBUTION_FILE, id: 'attribution', xhtml: attributionXhtml(pack, prepared, lang, today) },
  ];

  // Navigation: index, each article with its headings, then the generated pages.
  const navItems: NavItem[] = [{ level: 0, label: 'Title page', href: `articles/${INDEX_FILE}` }];
  for (const p of prepared) {
    navItems.push({ level: 0, label: p.title, href: `articles/${p.fileName}` });
    for (const h of headingsIn(p.nodes)) {
      navItems.push({
        level: Math.max(1, Number(h.localName[1])),
        label: headingLabel(h, 'Section'),
        href: `articles/${p.fileName}#${h.getAttribute('id')}`,
      });
    }
  }
  navItems.push({ level: 0, label: 'External links', href: `articles/${EXTERNAL_FILE}` });
  navItems.push({ level: 0, label: 'Missing linked articles', href: `articles/${MISSING_FILE}` });
  navItems.push({ level: 0, label: 'Sources and attribution', href: `articles/${ATTRIBUTION_FILE}` });
  const nav = buildTree(navItems);

  const book: Book = {
    identifier: bookIdentifier(pack.id),
    title,
    creator: pack.kind === 'wikipedia' ? 'Wikipedia contributors' : 'Various',
    language: lang,
    date: today,
    sources: pack.rootUrl ? [pack.rootUrl] : [],
    subjects: ['K-rosspoint pack'],
    css: epubCss(style) + PACK_CSS,
    chapters: [],
    nav,
    images: [],
  };
  const css = book.css;
  const opfFixed = { nav: navXhtml(nav, lang, CSS_PATH), ncx: ncxXml(book) };

  // Writes the ZIP. Pages in `noImages` get no images.
  const assemble = async (noImages: Set<PreparedPage>): Promise<Uint8Array> => {
    const articleFiles: TextFile[] = prepared.map((p, i) => ({
      file: p.fileName,
      id: `art-${pad(i + 1, 3)}`,
      xhtml: articleXhtml(p, lang, !noImages.has(p), fileIds, renames),
    }));
    const files = [indexFile, ...articleFiles, ...tail];
    const imgs = prepared.filter((p) => !noImages.has(p)).flatMap((p) => p.images);
    const zip = new JSZip();
    zip.file('mimetype', EPUB_MIME, { compression: 'STORE' });
    zip.file('META-INF/container.xml', CONTAINER_XML);
    zip.file(`${ROOT}/package.opf`, packOpf(book, files, imgs));
    zip.file(`${ROOT}/nav.xhtml`, opfFixed.nav);
    zip.file(`${ROOT}/toc.ncx`, opfFixed.ncx);
    zip.file(`${ROOT}/${CSS_PATH}`, css);
    for (const f of files) zip.file(`${ROOT}/articles/${f.file}`, f.xhtml);
    for (const im of imgs) zip.file(`${ROOT}/${im.path}`, im.blob, { binary: true });
    return zip.generateAsync({
      type: 'uint8array',
      compression: 'DEFLATE',
      compressionOptions: { level: 9 },
      mimeType: EPUB_MIME,
    });
  };

  // Size limit: remove the images of the largest pages first.
  const limitMB = pack.limits?.maxTotalEpubSizeMB;
  const limitBytes = Number.isFinite(limitMB) && (limitMB as number) > 0 ? (limitMB as number) * MB : Infinity;
  const noImages = new Set<PreparedPage>();
  const byImageSize = prepared.filter((p) => p.images.length > 0).sort((a, b) => b.imageBytes - a.imageBytes);
  let bytes = await assemble(noImages);
  while (bytes.byteLength > limitBytes) {
    const next = byImageSize.find((p) => !noImages.has(p));
    if (!next) break;
    noImages.add(next);
    bytes = await assemble(noImages);
  }
  if (bytes.byteLength > limitBytes) {
    const mb = (n: number) => (Math.round((n / MB) * 100) / 100).toString();
    const message = `Pack is ${mb(bytes.byteLength)} MB; the limit is ${limitMB} MB. Lower the article count or the image limit.`;
    progress(onProgress, { phase: 'failed', done: pages.length, total: pages.length, message }, errors);
    throw new Error(message);
  }

  const dropped = noImages.size;
  progress(
    onProgress,
    {
      phase: 'complete',
      done: pages.length,
      total: pages.length,
      message: dropped ? `Removed the images of ${dropped} article(s) to stay under the size limit.` : undefined,
    },
    errors,
  );
  return {
    name: `${sanitizeFilename(title)}.epub`,
    mime: EPUB_MIME,
    blob: bytesToBlob(bytes, EPUB_MIME),
    format: 'epub',
    estimatedPages: Math.max(1, estimatedPages),
  };
}
