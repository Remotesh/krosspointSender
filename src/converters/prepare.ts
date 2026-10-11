/**
 * Body preparation and safe DOM copy.
 * Every output format starts here:
 * 1. prepareBody() parses Article.bodyHtml into an HTML document. It applies
 *    the table and link settings and makes all ids valid and unique.
 * 2. copyNodes() copies the prepared nodes into a target document (XHTML for
 *    the EPUB, HTML for the HTML file). It keeps only allowed tags and
 *    attributes. So the output is always well-formed.
 */
import type { Article, StyleSettings } from '../types';
import { XHTML_NS, cleanXmlText, collapseWs } from './util';

export interface PreparedBody {
  doc: Document;
  /** The content root. Its direct children are the top-level blocks. */
  root: Element;
}

const DROP = new Set([
  'script', 'style', 'iframe', 'object', 'embed', 'noscript', 'template', 'form', 'input',
  'button', 'select', 'textarea', 'option', 'svg', 'math', 'video', 'audio', 'canvas', 'map',
  'area', 'source', 'track', 'head', 'title', 'meta', 'link', 'base', 'nav', 'dialog', 'frame',
  'frameset', 'param', 'applet',
]);

/** Tags that we copy with the same name. */
const KEEP = new Set([
  'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'ul', 'ol', 'li', 'blockquote', 'pre', 'code',
  'b', 'strong', 'i', 'em', 'u', 'del', 'sub', 'sup', 'span', 'div', 'section',
  'table', 'thead', 'tbody', 'tfoot', 'tr', 'th', 'td', 'caption', 'dl', 'dt', 'dd',
  'ruby', 'rt', 'rp',
]);

/** Tags that we copy with a different name. CrossPoint knows the target names. */
const RENAME: Record<string, string> = {
  article: 'div', main: 'div', header: 'div', footer: 'div', center: 'div', address: 'div',
  details: 'div', summary: 'p', hgroup: 'div', aside: 'div',
  cite: 'i', dfn: 'i', var: 'i', q: 'span', kbd: 'code', samp: 'code', tt: 'code',
  s: 'del', strike: 'del', ins: 'u', mark: 'span', small: 'span', big: 'span', abbr: 'span',
  time: 'span', font: 'span', bdi: 'span', bdo: 'span', data: 'span', label: 'span',
};

/** Parent tags that can hold a <p>. In other parents we use <span>. */
const BLOCK_PARENTS = new Set([
  'body', 'div', 'section', 'blockquote', 'li', 'td', 'th', 'dd', 'figure', 'caption', 'main',
  'header', 'article',
]);

const BLOCK_TAGS = new Set([
  'p', 'div', 'section', 'ul', 'ol', 'li', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'blockquote',
  'pre', 'table', 'dl', 'figure', 'hr', 'article', 'main', 'header', 'footer', 'aside',
]);

const VALID_ID = /^[A-Za-z_][A-Za-z0-9_.-]*$/;

function fixId(raw: string): string {
  let id = raw.trim().replace(/[^A-Za-z0-9_.-]+/g, '-').replace(/^-+|-+$/g, '');
  if (!id) id = 'id';
  if (!VALID_ID.test(id)) id = `id-${id}`;
  return id;
}

function unwrap(el: Element): void {
  const parent = el.parentNode;
  if (!parent) return;
  while (el.firstChild) parent.insertBefore(el.firstChild, el);
  parent.removeChild(el);
}

function hasBlockChild(el: Element): boolean {
  for (const c of Array.from(el.children)) {
    if (BLOCK_TAGS.has(c.localName.toLowerCase())) return true;
  }
  return false;
}

/** Replaces a table with one paragraph per row. Cells are joined with " | ". */
function linearizeTable(doc: Document, table: HTMLTableElement): void {
  const box = doc.createElement('div');
  box.className = 'table-linear';
  const cap = table.caption;
  if (cap && collapseWs(cap.textContent ?? '')) {
    const p = doc.createElement('p');
    const b = doc.createElement('strong');
    b.textContent = collapseWs(cap.textContent ?? '');
    p.appendChild(b);
    box.appendChild(p);
  }
  for (const row of Array.from(table.rows)) {
    const p = doc.createElement('p');
    let first = true;
    for (const cell of Array.from(row.cells)) {
      const text = collapseWs(cell.textContent ?? '');
      const imgs = cell.querySelectorAll('img');
      if (!text && imgs.length === 0) continue;
      if (!first) p.appendChild(doc.createTextNode(' | '));
      first = false;
      const holder = cell.localName === 'th' ? doc.createElement('strong') : doc.createDocumentFragment();
      if (hasBlockChild(cell)) {
        holder.appendChild(doc.createTextNode(text));
        imgs.forEach((im) => holder.appendChild(im.cloneNode(false)));
      } else {
        while (cell.firstChild) holder.appendChild(cell.firstChild);
      }
      p.appendChild(holder);
    }
    if (p.childNodes.length) box.appendChild(p);
  }
  table.replaceWith(box);
}

/** Makes a "<p><b>text</b></p>" from a <dt>. The dt content moves into the <b>. */
function dtToParagraph(doc: Document, dt: Element): HTMLParagraphElement {
  const p = doc.createElement('p');
  const b = doc.createElement('b');
  while (dt.firstChild) b.appendChild(dt.firstChild);
  p.appendChild(b);
  return p;
}

/**
 * Makes a <dl> valid for XHTML: each group is dt+ dd+.
 * Wikipedia uses "<dl><dt>" without <dd> as a pseudo-heading.
 * - A dl without a dd becomes one "<p><b>dt text</b></p>" per dt.
 * - A dd without a dt before it gets an empty dt.
 * - Trailing dt elements without a dd become paragraphs after the dl.
 * Other children of the dl move out of it, in place.
 */
function fixDefinitionList(doc: Document, dl: Element): void {
  const parent = dl.parentNode;
  if (!parent) return;
  const kids = Array.from(dl.children);
  if (!kids.some((k) => k.localName.toLowerCase() === 'dd')) {
    for (const k of Array.from(dl.childNodes)) {
      const isDt = k.nodeType === 1 && (k as Element).localName.toLowerCase() === 'dt';
      parent.insertBefore(isDt ? dtToParagraph(doc, k as Element) : k, dl);
    }
    dl.remove();
    return;
  }
  let prev: 'none' | 'dt' | 'dd' = 'none';
  let trailing: Element[] = [];
  for (const k of kids) {
    const tag = k.localName.toLowerCase();
    if (tag === 'dt') {
      if (prev !== 'dt') trailing = [];
      trailing.push(k);
      prev = 'dt';
    } else if (tag === 'dd') {
      if (prev === 'none') dl.insertBefore(doc.createElement('dt'), k);
      trailing = [];
      prev = 'dd';
    } else if (tag === 'div' && k.querySelector(':scope > dt, :scope > dd')) {
      // An HTML div group: keep it, but check it as its own list.
      prev = 'none';
      trailing = [];
    } else {
      // Not allowed in a dl. Move it before the dl.
      parent.insertBefore(k, dl);
    }
  }
  if (prev === 'dt') {
    const after = dl.nextSibling;
    for (const dt of trailing) {
      parent.insertBefore(dtToParagraph(doc, dt), after);
      dt.remove();
    }
  }
  // Loose text in a dl is not allowed. Move it out.
  for (const n of Array.from(dl.childNodes)) {
    if (n.nodeType === 3 && (n.textContent ?? '').trim() !== '') parent.insertBefore(n, dl);
  }
  if (dl.children.length === 0) dl.remove();
}

/**
 * Parses the article body and applies the style rules.
 * - tableHandling: keep, linearize (one paragraph per row) or drop.
 * - keepLinks false: links become plain text.
 * - All ids become valid XML ids and unique. Headings without an id get "sec-N".
 *   Internal "#id" links follow the new ids.
 * - Single wrapper elements (div/section/article/main) are removed, so the
 *   root holds the top-level blocks.
 */
export function prepareBody(article: Article, style: StyleSettings): PreparedBody {
  const doc = new DOMParser().parseFromString(
    '<!DOCTYPE html><html><head><meta charset="utf-8"><title></title></head><body></body></html>',
    'text/html',
  );
  doc.body.innerHTML = article.bodyHtml ?? '';
  const body = doc.body;

  body.querySelectorAll(Array.from(DROP).join(',')).forEach((el) => el.remove());

  // Definition lists. Work from the last list, so inner lists go first.
  Array.from(body.querySelectorAll('dl')).reverse().forEach((dl) => fixDefinitionList(doc, dl));

  // Tables. Work from the last table, so inner tables go first.
  const tables = Array.from(body.querySelectorAll('table')).reverse();
  for (const t of tables) {
    if (!t.isConnected) continue;
    if (style.tableHandling === 'drop') t.remove();
    else if (style.tableHandling === 'linearize') linearizeTable(doc, t as HTMLTableElement);
  }

  // Ids: valid and unique.
  const used = new Set<string>();
  const remap = new Map<string, string>();
  const unique = (base: string): string => {
    let id = base;
    let n = 2;
    while (used.has(id)) id = `${base}-${n++}`;
    used.add(id);
    return id;
  };
  body.querySelectorAll('[id]').forEach((el) => {
    const raw = el.getAttribute('id') ?? '';
    const id = unique(fixId(raw));
    if (!remap.has(raw)) remap.set(raw, id);
    el.setAttribute('id', id);
  });
  let sec = 0;
  body.querySelectorAll('h1,h2,h3,h4,h5,h6').forEach((h) => {
    if (h.getAttribute('id')) return;
    let id: string;
    do id = `sec-${++sec}`;
    while (used.has(id));
    used.add(id);
    h.setAttribute('id', id);
  });

  // Links. Internal "#id" links stay, so the reader can jump inside the book.
  body.querySelectorAll('a').forEach((a) => {
    const href = (a.getAttribute('href') ?? '').trim();
    if (href.startsWith('#')) {
      let frag = href.slice(1);
      try {
        frag = decodeURIComponent(frag);
      } catch {
        // Keep the raw fragment.
      }
      const target = remap.get(frag) ?? (used.has(frag) ? frag : undefined);
      if (target) a.setAttribute('href', `#${target}`);
      else unwrap(a);
      return;
    }
    if (!style.keepLinks) unwrap(a);
  });

  // Remove single wrappers.
  let root: Element = body;
  for (;;) {
    const kids = Array.from(root.children);
    const loose = Array.from(root.childNodes).some(
      (n) => n.nodeType === 3 && (n.textContent ?? '').trim() !== '',
    );
    if (kids.length !== 1 || loose) break;
    const only = kids[0];
    if (!['div', 'section', 'article', 'main'].includes(only.localName.toLowerCase())) break;
    root = only;
  }
  return { doc, root };
}

// ───────────────────────── Safe copy ─────────────────────────

export interface CopyContext {
  /**
   * Maps an <img> to the output src. Returns null to drop the image.
   * A dropped image leaves its alt text in a "img-alt" paragraph or span.
   */
  image(src: string, alt: string): string | null;
  /** Maps a link href. Returns null to keep only the link text. */
  href(raw: string): string | null;
}

function textOk(s: string): string {
  return cleanXmlText(s);
}

function el(doc: Document, tag: string, cls?: string): Element {
  const e = doc.createElementNS(XHTML_NS, tag);
  if (cls) e.setAttribute('class', cls);
  return e;
}

function blockParent(parent: Element): boolean {
  return BLOCK_PARENTS.has(parent.localName.toLowerCase());
}

function copyId(src: Element, out: Element): void {
  const id = src.getAttribute('id');
  if (id && VALID_ID.test(id)) out.setAttribute('id', id);
}

function copySpan(src: Element, out: Element): void {
  for (const name of ['colspan', 'rowspan']) {
    const v = src.getAttribute(name);
    if (v && /^\d{1,3}$/.test(v) && Number(v) > 0) out.setAttribute(name, v);
  }
}

/** Copies one node and its subtree into outParent. */
export function copyNode(node: Node, outParent: Element, ctx: CopyContext): void {
  const doc = outParent.ownerDocument as Document;
  if (node.nodeType === 3) {
    const t = textOk(node.nodeValue ?? '');
    if (t) outParent.appendChild(doc.createTextNode(t));
    return;
  }
  if (node.nodeType === 11) {
    node.childNodes.forEach((c) => copyNode(c, outParent, ctx));
    return;
  }
  if (node.nodeType !== 1) return;
  const src = node as Element;
  const tag = src.localName.toLowerCase();
  if (DROP.has(tag)) return;

  if (tag === 'img') {
    const alt = collapseWs(src.getAttribute('alt') ?? '');
    const out = ctx.image((src.getAttribute('src') ?? '').trim(), alt);
    if (out) {
      const img = el(doc, 'img');
      img.setAttribute('src', out);
      img.setAttribute('alt', textOk(alt));
      copyId(src, img);
      outParent.appendChild(img);
    } else if (alt) {
      const holder = el(doc, blockParent(outParent) ? 'p' : 'span', 'img-alt');
      holder.appendChild(doc.createTextNode(textOk(alt)));
      outParent.appendChild(holder);
    }
    return;
  }
  if (tag === 'br' || tag === 'hr') {
    const out = el(doc, tag);
    outParent.appendChild(out);
    return;
  }
  if (tag === 'wbr') return;
  if (tag === 'a') {
    const href = ctx.href((src.getAttribute('href') ?? '').trim());
    if (href === null) {
      src.childNodes.forEach((c) => copyNode(c, outParent, ctx));
      return;
    }
    const a = el(doc, 'a');
    a.setAttribute('href', textOk(href));
    copyId(src, a);
    // Keep the pack marker class so CSS can style links that stay online.
    if (src.classList.contains('ext')) a.setAttribute('class', 'ext');
    outParent.appendChild(a);
    src.childNodes.forEach((c) => copyNode(c, a, ctx));
    return;
  }
  if (tag === 'figure') {
    const out = el(doc, 'div', 'figure');
    copyId(src, out);
    outParent.appendChild(out);
    src.childNodes.forEach((c) => copyNode(c, out, ctx));
    return;
  }
  if (tag === 'figcaption') {
    // A caption that holds a table or list cannot sit inside <p>. Use <div> then.
    const out = el(doc, blockParent(outParent) ? (hasBlockChild(src) ? 'div' : 'p') : 'span', 'caption');
    copyId(src, out);
    outParent.appendChild(out);
    src.childNodes.forEach((c) => copyNode(c, out, ctx));
    return;
  }
  const name = KEEP.has(tag) ? tag : RENAME[tag];
  if (!name) {
    // Unknown tag: keep its content only.
    src.childNodes.forEach((c) => copyNode(c, outParent, ctx));
    return;
  }
  const out = el(doc, name);
  const cls = src.getAttribute('class');
  // Keep only the classes that our own steps add.
  if (cls && /^(img-alt|caption|figure|table-linear|ext|source)$/.test(cls)) out.setAttribute('class', cls);
  copyId(src, out);
  if (name === 'td' || name === 'th') copySpan(src, out);
  outParent.appendChild(out);
  src.childNodes.forEach((c) => copyNode(c, out, ctx));
}

/** Copies a list of nodes into outParent. */
export function copyNodes(nodes: Iterable<Node>, outParent: Element, ctx: CopyContext): void {
  for (const n of nodes) copyNode(n, outParent, ctx);
}

/** Default href rule: keep http(s), mailto and "#" links. Drop the rest. */
export function defaultHref(raw: string): string | null {
  if (!raw) return null;
  if (raw.startsWith('#')) return raw.length > 1 ? raw : null;
  if (/^(https?:|mailto:)/i.test(raw)) return raw;
  return null;
}

/** Adds the cover/title block (title + label lines) to a parent element. */
export function appendTitleBlock(
  parent: Element,
  title: string,
  lines: { label: string; value: string }[],
  headingTag = 'h1',
): void {
  const doc = parent.ownerDocument as Document;
  const h = el(doc, headingTag, 'title');
  h.appendChild(doc.createTextNode(textOk(title || 'Untitled')));
  parent.appendChild(h);
  for (const line of lines) {
    const p = el(doc, 'p', 'meta');
    const b = el(doc, 'b');
    b.appendChild(doc.createTextNode(`${line.label}: `));
    p.appendChild(b);
    p.appendChild(doc.createTextNode(textOk(line.value)));
    parent.appendChild(p);
  }
}

/** Adds the source note section to a parent element. */
export function appendSourceNote(parent: Element, text: string): void {
  const doc = parent.ownerDocument as Document;
  const section = el(doc, 'section', 'source-note');
  section.appendChild(el(doc, 'hr'));
  const p = el(doc, 'p');
  p.appendChild(doc.createTextNode(textOk(text)));
  section.appendChild(p);
  parent.appendChild(section);
}

/** Creates an element in the XHTML namespace. */
export function xel(doc: Document, tag: string, cls?: string): Element {
  return el(doc, tag, cls);
}
