/**
 * EPUB 3 builder with EPUB 2 fallbacks (NCX, guide).
 * Layout:
 *   mimetype                    (first entry, stored)
 *   META-INF/container.xml
 *   OEBPS/content.opf
 *   OEBPS/nav.xhtml             (EPUB 3 nav, epub:type="toc")
 *   OEBPS/toc.ncx               (EPUB 2 NCX)
 *   OEBPS/style.css
 *   OEBPS/cover.xhtml           (text title page, when includeCover)
 *   OEBPS/chapter-001.xhtml ...
 *   OEBPS/images/img-001.jpg ...
 * We build every XHTML file through the DOM and XMLSerializer.
 * So each file is well-formed XML.
 */
import JSZip from 'jszip';
import type { Article, BuildOptions, OutputFile, StyleSettings } from '../types';
import { processImage } from '../converters/images';
import {
  appendSourceNote,
  appendTitleBlock,
  copyNodes,
  defaultHref,
  prepareBody,
  xel,
  type CopyContext,
  type PreparedBody,
} from '../converters/prepare';
import { estimateReading } from '../converters/reading';
import { sanitizeFilename } from '../converters/filename';
import {
  APP_NAME,
  EPUB_NS,
  XHTML_NS,
  XMLNS_NS,
  XML_NS,
  bookIdentifier,
  bytesToBlob,
  collapseWs,
  coverLines,
  escapeXml,
  isoDay,
  langOf,
  modifiedStamp,
  pad,
  randomUuid,
  sourceNoteText,
} from '../converters/util';
import { epubCss, PRESETS } from './css';

export const EPUB_MIME = 'application/epub+zip';

// ───────────────────────── Types ─────────────────────────

export interface NavNode {
  label: string;
  href: string;
  children: NavNode[];
}

export interface NavItem {
  level: number;
  label: string;
  href: string;
}

export interface ImageFile {
  /** Path inside OEBPS, e.g. "images/img-001.jpg". */
  path: string;
  id: string;
  mime: string;
  blob: Blob;
}

export interface ChapterFile {
  file: string;
  id: string;
  xhtml: string;
}

export interface Book {
  identifier: string;
  title: string;
  creator?: string;
  language: string;
  date: string;
  sources: string[];
  subjects: string[];
  description?: string;
  publisher?: string;
  css: string;
  cover?: string;
  chapters: ChapterFile[];
  nav: NavNode[];
  images: ImageFile[];
}

// ───────────────────────── XHTML documents ─────────────────────────

/** Creates an XHTML document with head, title and the stylesheet link. */
export function newXhtml(title: string, lang: string, cssHref = 'style.css'): { doc: Document; body: Element } {
  const doc = document.implementation.createDocument(XHTML_NS, 'html', null);
  const html = doc.documentElement;
  html.setAttributeNS(XMLNS_NS, 'xmlns:epub', EPUB_NS);
  html.setAttribute('lang', lang);
  html.setAttributeNS(XML_NS, 'xml:lang', lang);
  const head = xel(doc, 'head');
  const meta = xel(doc, 'meta');
  meta.setAttribute('charset', 'utf-8');
  head.appendChild(meta);
  const t = xel(doc, 'title');
  t.appendChild(doc.createTextNode(collapseWs(title) || 'Untitled'));
  head.appendChild(t);
  const link = xel(doc, 'link');
  link.setAttribute('rel', 'stylesheet');
  link.setAttribute('type', 'text/css');
  link.setAttribute('href', cssHref);
  head.appendChild(link);
  html.appendChild(head);
  const body = xel(doc, 'body');
  html.appendChild(body);
  return { doc, body };
}

export function serializeXhtml(doc: Document): string {
  const xml = new XMLSerializer().serializeToString(doc.documentElement);
  return `<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n${xml}\n`;
}

// ───────────────────────── Images ─────────────────────────

/**
 * Processes the images of one article. Returns the src -> local path map
 * and the files to write. Images without a blob or with a failed decode are
 * not in the map, so the copy step drops them.
 */
async function collectImages(
  prepared: PreparedBody,
  opts: BuildOptions,
  images: Map<string, Blob> | undefined,
  baseName: (n: number) => string,
): Promise<{ map: Map<string, string>; files: ImageFile[] }> {
  const map = new Map<string, string>();
  const files: ImageFile[] = [];
  if (!opts.includeImages || !images || images.size === 0) return { map, files };
  const style = opts.style;
  const srcs: string[] = [];
  prepared.root.querySelectorAll('img').forEach((img) => {
    const s = (img.getAttribute('src') ?? '').trim();
    if (s && !srcs.includes(s)) srcs.push(s);
  });
  let n = 0;
  for (const src of srcs) {
    const blob = images.get(src);
    if (!blob) continue;
    const out = await processImage(blob, {
      maxWidth: style.imageMaxWidthPx > 0 ? Math.min(style.imageMaxWidthPx, 2000) : 480,
      grayscale: style.grayscaleImages,
      quality: style.imageQuality,
    });
    if (!out) continue;
    n++;
    const base = baseName(n);
    const path = `images/${base}.${out.mime === 'image/png' ? 'png' : 'jpg'}`;
    map.set(src, path);
    files.push({ path, id: base, mime: out.mime, blob: out.blob });
  }
  return { map, files };
}

// ───────────────────────── Chapters ─────────────────────────

interface Chunk {
  heading?: Element;
  nodes: Node[];
}

function hasContent(nodes: Node[]): boolean {
  return nodes.some((n) => {
    if (n.nodeType === 3) return (n.textContent ?? '').trim() !== '';
    if (n.nodeType !== 1) return false;
    const e = n as Element;
    return (e.textContent ?? '').trim() !== '' || !!e.querySelector('img,hr,table') || /^(img|hr|table)$/i.test(e.localName);
  });
}

/** Splits at each top-level h1/h2 when there is more than one of them. */
function splitChunks(root: Element): Chunk[] {
  const nodes = Array.from(root.childNodes);
  const isSplit = (n: Node) => n.nodeType === 1 && /^h[12]$/i.test((n as Element).localName);
  const count = nodes.filter(isSplit).length;
  if (count <= 1) return [{ nodes }];
  const chunks: Chunk[] = [];
  let cur: Chunk = { nodes: [] };
  for (const n of nodes) {
    if (isSplit(n)) {
      if (cur.heading || hasContent(cur.nodes)) chunks.push(cur);
      cur = { heading: n as Element, nodes: [n] };
    } else {
      cur.nodes.push(n);
    }
  }
  if (cur.heading || hasContent(cur.nodes)) chunks.push(cur);
  return chunks;
}

export function headingsIn(nodes: Node[], maxLevel = 4): Element[] {
  const out: Element[] = [];
  for (const n of nodes) {
    if (n.nodeType !== 1) continue;
    const e = n as Element;
    if (/^h[1-6]$/i.test(e.localName)) out.push(e);
    e.querySelectorAll('h1,h2,h3,h4,h5,h6').forEach((h) => out.push(h));
  }
  return out.filter((h) => Number(h.localName[1]) <= maxLevel && h.getAttribute('id'));
}

export function idsIn(nodes: Node[]): string[] {
  const out: string[] = [];
  for (const n of nodes) {
    if (n.nodeType !== 1) continue;
    const e = n as Element;
    const id = e.getAttribute('id');
    if (id) out.push(id);
    e.querySelectorAll('[id]').forEach((x) => out.push(x.getAttribute('id') as string));
  }
  return out;
}

export function headingLabel(h: Element, fallback: string): string {
  return collapseWs(h.textContent ?? '') || fallback;
}

export function buildTree(items: NavItem[]): NavNode[] {
  const root: NavNode[] = [];
  const stack: { level: number; node: NavNode }[] = [];
  for (const it of items) {
    const node: NavNode = { label: it.label, href: it.href, children: [] };
    while (stack.length && stack[stack.length - 1].level >= it.level) stack.pop();
    (stack.length ? stack[stack.length - 1].node.children : root).push(node);
    stack.push({ level: it.level, node });
  }
  return root;
}

function makeCopyContext(
  imageMap: Map<string, string>,
  idToFile: Map<string, string>,
  currentFile: string,
  includeImages: boolean,
): CopyContext {
  return {
    image: (src) => (includeImages ? imageMap.get(src) ?? null : null),
    href: (raw) => {
      const h = defaultHref(raw);
      if (h === null) return null;
      if (h.startsWith('#')) {
        const id = h.slice(1);
        const file = idToFile.get(id);
        if (!file) return null;
        return file === currentFile ? h : `${file}${h}`;
      }
      return h;
    },
  };
}

function bylineText(article: Article): string {
  return [article.author, article.siteName, isoDay(article.publishedAt)]
    .map((x) => collapseWs(x ?? ''))
    .filter(Boolean)
    .join(' · ');
}

function appendByline(body: Element, article: Article): void {
  const text = bylineText(article);
  if (!text) return;
  const p = xel(body.ownerDocument as Document, 'p', 'byline');
  p.appendChild((body.ownerDocument as Document).createTextNode(text));
  body.appendChild(p);
}

/** Makes sure a body is not empty. */
export function ensureContent(body: Element): void {
  if (body.childNodes.length === 0) body.appendChild(xel(body.ownerDocument as Document, 'p'));
}

// ───────────────────────── Cover, nav, OPF, NCX ─────────────────────────

function coverXhtml(title: string, lang: string, fill: (cover: Element) => void): string {
  const { doc, body } = newXhtml(title, lang);
  const div = xel(doc, 'div', 'cover');
  div.setAttributeNS(EPUB_NS, 'epub:type', 'titlepage');
  fill(div);
  body.appendChild(div);
  return serializeXhtml(doc);
}

export function navXhtml(nav: NavNode[], lang: string, cssHref = 'style.css'): string {
  const { doc, body } = newXhtml('Contents', lang, cssHref);
  const navEl = xel(doc, 'nav');
  navEl.setAttributeNS(EPUB_NS, 'epub:type', 'toc');
  navEl.setAttribute('id', 'toc');
  const h = xel(doc, 'h1');
  h.appendChild(doc.createTextNode('Contents'));
  navEl.appendChild(h);
  const addList = (parent: Element, nodes: NavNode[]) => {
    const ol = xel(doc, 'ol');
    for (const n of nodes) {
      const li = xel(doc, 'li');
      const a = xel(doc, 'a');
      a.setAttribute('href', n.href);
      a.appendChild(doc.createTextNode(n.label));
      li.appendChild(a);
      if (n.children.length) addList(li, n.children);
      ol.appendChild(li);
    }
    parent.appendChild(ol);
  };
  addList(navEl, nav);
  body.appendChild(navEl);
  return serializeXhtml(doc);
}

function navDepth(nodes: NavNode[]): number {
  return nodes.reduce((d, n) => Math.max(d, 1 + navDepth(n.children)), 0);
}

export function ncxXml(book: Book): string {
  let order = 0;
  const points = (nodes: NavNode[], indent: string): string =>
    nodes
      .map((n) => {
        // Take this point's number before the children take theirs.
        const my = ++order;
        const kids = n.children.length ? `\n${points(n.children, `${indent}  `)}` : '';
        return (
          `${indent}<navPoint id="np-${my}" playOrder="${my}">\n` +
          `${indent}  <navLabel><text>${escapeXml(n.label)}</text></navLabel>\n` +
          `${indent}  <content src="${escapeXml(n.href)}"/>${kids}\n` +
          `${indent}</navPoint>`
        );
      })
      .join('\n');
  const map = points(book.nav, '    ');
  return `<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1" xml:lang="${escapeXml(book.language)}">
  <head>
    <meta name="dtb:uid" content="${escapeXml(book.identifier)}"/>
    <meta name="dtb:depth" content="${Math.max(1, navDepth(book.nav))}"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>${escapeXml(book.title)}</text></docTitle>
${book.creator ? `  <docAuthor><text>${escapeXml(book.creator)}</text></docAuthor>\n` : ''}  <navMap>
${map}
  </navMap>
</ncx>
`;
}

function opfXml(book: Book): string {
  const meta: string[] = [];
  meta.push(`    <dc:identifier id="bookid">${escapeXml(book.identifier)}</dc:identifier>`);
  meta.push(`    <dc:title>${escapeXml(book.title)}</dc:title>`);
  if (book.creator) meta.push(`    <dc:creator id="creator">${escapeXml(book.creator)}</dc:creator>`);
  meta.push(`    <dc:language>${escapeXml(book.language)}</dc:language>`);
  meta.push(`    <dc:date>${escapeXml(book.date)}</dc:date>`);
  for (const s of book.sources) meta.push(`    <dc:source>${escapeXml(s)}</dc:source>`);
  if (book.publisher) meta.push(`    <dc:publisher>${escapeXml(book.publisher)}</dc:publisher>`);
  if (book.description) meta.push(`    <dc:description>${escapeXml(book.description)}</dc:description>`);
  for (const t of book.subjects) meta.push(`    <dc:subject>${escapeXml(t)}</dc:subject>`);
  meta.push(`    <meta property="dcterms:modified">${modifiedStamp()}</meta>`);
  meta.push(`    <meta name="generator" content="${APP_NAME}"/>`);

  const items: string[] = [
    '    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
    '    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>',
    '    <item id="css" href="style.css" media-type="text/css"/>',
  ];
  if (book.cover) items.push('    <item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>');
  for (const c of book.chapters) {
    items.push(`    <item id="${c.id}" href="${c.file}" media-type="application/xhtml+xml"/>`);
  }
  for (const im of book.images) {
    items.push(`    <item id="${escapeXml(im.id)}" href="${escapeXml(im.path)}" media-type="${im.mime}"/>`);
  }
  const spine: string[] = [];
  if (book.cover) spine.push('    <itemref idref="cover"/>');
  for (const c of book.chapters) spine.push(`    <itemref idref="${c.id}"/>`);
  const guide: string[] = [];
  if (book.cover) guide.push('    <reference type="cover" title="Cover" href="cover.xhtml"/>');
  guide.push('    <reference type="toc" title="Contents" href="nav.xhtml"/>');
  if (book.chapters[0]) guide.push(`    <reference type="text" title="Start" href="${book.chapters[0].file}"/>`);

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

const CONTAINER_XML = `<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
`;

/** Writes the ZIP. The "mimetype" entry is first and stored. All other entries use DEFLATE. */
async function packEpub(book: Book): Promise<Uint8Array> {
  const zip = new JSZip();
  zip.file('mimetype', EPUB_MIME, { compression: 'STORE' });
  zip.file('META-INF/container.xml', CONTAINER_XML);
  zip.file('OEBPS/content.opf', opfXml(book));
  zip.file('OEBPS/nav.xhtml', navXhtml(book.nav, book.language));
  zip.file('OEBPS/toc.ncx', ncxXml(book));
  zip.file('OEBPS/style.css', book.css);
  if (book.cover) zip.file('OEBPS/cover.xhtml', book.cover);
  for (const c of book.chapters) zip.file(`OEBPS/${c.file}`, c.xhtml);
  for (const im of book.images) zip.file(`OEBPS/${im.path}`, im.blob, { binary: true });
  return zip.generateAsync({
    type: 'uint8array',
    compression: 'DEFLATE',
    compressionOptions: { level: 9 },
    mimeType: EPUB_MIME,
  });
}

function chapterFile(n: number): string {
  return `chapter-${pad(n, 3)}.xhtml`;
}

// ───────────────────────── Public API ─────────────────────────

/** Builds the EPUB bytes and the page estimate. buildZip uses this too. */
export async function buildEpubBytes(
  article: Article,
  opts: BuildOptions,
  images?: Map<string, Blob>,
): Promise<{ bytes: Uint8Array; pages: number }> {
  const style = opts.style;
  const lang = langOf(article);
  const title = collapseWs(article.title ?? '') || 'Untitled';
  const prepared = prepareBody(article, style);
  const imgs = await collectImages(prepared, opts, images, (n) => `img-${pad(n, 3)}`);

  const chunks = splitChunks(prepared.root);
  const split = chunks.length > 1 || !!chunks[0]?.heading;
  const files = chunks.map((_, i) => chapterFile(i + 1));
  const idToFile = new Map<string, string>();
  chunks.forEach((c, i) => idsIn(c.nodes).forEach((id) => idToFile.has(id) || idToFile.set(id, files[i])));

  // Navigation items.
  const navItems: NavItem[] = [];
  const allHeads = chunks.flatMap((c) => headingsIn(c.nodes));
  const minLevel = allHeads.reduce((m, h) => Math.min(m, Number(h.localName[1])), 6);
  chunks.forEach((c, i) => {
    if (!c.heading) {
      // A preface chunk, or the only chunk.
      navItems.push({ level: split ? minLevel : 0, label: title, href: files[i] });
    }
    for (const h of headingsIn(c.nodes)) {
      const id = h.getAttribute('id') as string;
      const href = h === c.heading ? files[i] : `${files[i]}#${id}`;
      navItems.push({ level: Number(h.localName[1]), label: headingLabel(h, 'Section'), href });
    }
  });

  const chapters: ChapterFile[] = chunks.map((c, i) => {
    const chTitle = c.heading ? headingLabel(c.heading, title) : title;
    const { doc, body } = newXhtml(chTitle, lang);
    if (i === 0 && !opts.includeCover) {
      appendTitleBlock(body, title, []);
      appendByline(body, article);
    }
    copyNodes(c.nodes, body, makeCopyContext(imgs.map, idToFile, files[i], opts.includeImages));
    if (i === chunks.length - 1 && opts.includeSourceNote) appendSourceNote(body, sourceNoteText(article));
    ensureContent(body);
    return { file: files[i], id: `chapter-${pad(i + 1, 3)}`, xhtml: serializeXhtml(doc) };
  });

  const nav = buildTree(navItems);
  let cover: string | undefined;
  if (opts.includeCover) {
    cover = coverXhtml(title, lang, (div) => appendTitleBlock(div, title, coverLines(article)));
    nav.unshift({ label: 'Title page', href: 'cover.xhtml', children: [] });
  }

  const book: Book = {
    identifier: bookIdentifier(article.id),
    title,
    creator: collapseWs(article.author ?? '') || undefined,
    language: lang,
    date: isoDay(article.publishedAt) || isoDay(article.capturedAt) || isoDay(new Date().toISOString()),
    sources: article.sourceUrl ? [article.sourceUrl] : [],
    subjects: (article.tags ?? []).map((t) => collapseWs(t)).filter(Boolean),
    description: collapseWs(article.excerpt ?? '') || undefined,
    publisher: collapseWs(article.siteName ?? '') || undefined,
    css: epubCss(style),
    cover,
    chapters,
    nav,
    images: imgs.files,
  };
  const bytes = await packEpub(book);
  return { bytes, pages: estimateReading(article, style).pages };
}

/**
 * Builds one EPUB for one article.
 * The body splits into chapter files at each top-level h1/h2 when there
 * is more than one. The `images` map holds blobs keyed by ArticleImage.src.
 * An image without a usable blob becomes its alt text.
 */
export async function buildEpub(article: Article, opts: BuildOptions, images?: Map<string, Blob>): Promise<OutputFile> {
  const { bytes, pages } = await buildEpubBytes(article, opts, images);
  return {
    name: `${sanitizeFilename(article.title)}.epub`,
    mime: EPUB_MIME,
    blob: bytesToBlob(bytes, EPUB_MIME),
    format: 'epub',
    estimatedPages: pages,
  };
}

/**
 * Builds one EPUB from many articles. Each article is one chapter file
 * (chapter-001.xhtml ... in article order). The nav lists each article and
 * its headings. The cover lists all titles. Image names carry the article
 * number, e.g. img-a01-003.jpg.
 */
export async function buildBundleEpub(
  articles: Article[],
  meta: { title: string; author?: string },
  opts: BuildOptions,
  images?: Map<string, Blob>,
): Promise<OutputFile> {
  const style = opts.style;
  const bookTitle = collapseWs(meta.title ?? '') || 'Articles';
  const lang = articles[0] ? langOf(articles[0]) : 'en';
  const chapters: ChapterFile[] = [];
  const navItems: NavItem[] = [];
  const imageFiles: ImageFile[] = [];
  let pages = 0;

  for (let ai = 0; ai < articles.length; ai++) {
    const article = articles[ai];
    const file = chapterFile(ai + 1);
    const title = collapseWs(article.title ?? '') || `Article ${ai + 1}`;
    const prepared = prepareBody(article, style);
    const imgs = await collectImages(prepared, opts, images, (n) => `img-a${pad(ai + 1, 2)}-${pad(n, 3)}`);
    imageFiles.push(...imgs.files);
    const nodes = Array.from(prepared.root.childNodes);
    const idToFile = new Map<string, string>();
    idsIn(nodes).forEach((id) => idToFile.set(id, file));

    navItems.push({ level: 0, label: title, href: file });
    for (const h of headingsIn(nodes)) {
      navItems.push({
        level: Number(h.localName[1]),
        label: headingLabel(h, 'Section'),
        href: `${file}#${h.getAttribute('id')}`,
      });
    }

    const { doc, body } = newXhtml(title, langOf(article));
    appendTitleBlock(body, title, []);
    appendByline(body, article);
    copyNodes(nodes, body, makeCopyContext(imgs.map, idToFile, file, opts.includeImages));
    if (opts.includeSourceNote) appendSourceNote(body, sourceNoteText(article));
    chapters.push({ file, id: `chapter-${pad(ai + 1, 3)}`, xhtml: serializeXhtml(doc) });
    pages += estimateReading(article, style).pages;
  }

  if (chapters.length === 0) {
    const { doc, body } = newXhtml(bookTitle, lang);
    appendTitleBlock(body, bookTitle, []);
    const p = xel(doc, 'p');
    p.appendChild(doc.createTextNode('This bundle has no articles.'));
    body.appendChild(p);
    chapters.push({ file: chapterFile(1), id: 'chapter-001', xhtml: serializeXhtml(doc) });
    navItems.push({ level: 0, label: bookTitle, href: chapterFile(1) });
    pages = 1;
  }

  const nav = buildTree(navItems);
  const author = collapseWs(meta.author ?? '');
  const today = isoDay(new Date().toISOString());
  let cover: string | undefined;
  if (opts.includeCover) {
    cover = coverXhtml(bookTitle, lang, (div) => {
      const lines = [];
      if (author) lines.push({ label: 'Author', value: author });
      lines.push({ label: 'Articles', value: String(articles.length) });
      lines.push({ label: 'Created', value: today });
      appendTitleBlock(div, bookTitle, lines);
      if (articles.length) {
        const doc = div.ownerDocument as Document;
        const ol = xel(doc, 'ol');
        articles.forEach((a, i) => {
          const li = xel(doc, 'li');
          li.appendChild(doc.createTextNode(collapseWs(a.title ?? '') || `Article ${i + 1}`));
          ol.appendChild(li);
        });
        div.appendChild(ol);
      }
    });
    nav.unshift({ label: 'Title page', href: 'cover.xhtml', children: [] });
  }

  const sources = articles.map((a) => a.sourceUrl ?? '').filter(Boolean);
  const subjects = Array.from(new Set(articles.flatMap((a) => (a.tags ?? []).map((t) => collapseWs(t)).filter(Boolean))));
  const book: Book = {
    identifier: `urn:uuid:${randomUuid()}`,
    title: bookTitle,
    creator: author || undefined,
    language: lang,
    date: today,
    sources: Array.from(new Set(sources)),
    subjects,
    css: epubCss(style),
    cover,
    chapters,
    nav,
    images: imageFiles,
  };
  const bytes = await packEpub(book);
  return {
    name: `${sanitizeFilename(bookTitle)}.epub`,
    mime: EPUB_MIME,
    blob: bytesToBlob(bytes, EPUB_MIME),
    format: 'epub',
    estimatedPages: Math.max(1, pages),
  };
}

/** A tiny valid EPUB to check the device transfer. The file is "krosspoint-test.epub". */
export async function sendTestEpub(): Promise<OutputFile> {
  const now = new Date().toISOString();
  const article: Article = {
    id: randomUuid(),
    title: 'K-rosspointSender test',
    author: APP_NAME,
    capturedAt: now,
    language: 'en',
    tags: [],
    bodyHtml:
      '<p>The transfer works. K-rosspointSender sent this book to your reader. ' +
      'You can delete this file now.</p>',
    toc: [],
    images: [],
    wordCount: 16,
    inputKind: 'text',
  };
  const style: StyleSettings = { ...PRESETS.comfortable };
  const { bytes } = await buildEpubBytes(article, {
    style,
    includeImages: false,
    includeCover: false,
    includeSourceNote: false,
  });
  return {
    name: 'krosspoint-test.epub',
    mime: EPUB_MIME,
    blob: bytesToBlob(bytes, EPUB_MIME),
    format: 'epub',
    estimatedPages: 1,
  };
}
