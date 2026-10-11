/**
 * Plain text output.
 * - Headings are underlined: "=" for h1, "-" for h2..h6.
 * - Lists use "- " (ordered lists use "1. ").
 * - A blank line separates paragraphs. Lines are not wrapped.
 * - Images become "[Image: alt]". Links become "text (url)" when keepLinks is on.
 */
import type { Article, BuildOptions, OutputFile } from '../types';
import { sanitizeFilename } from './filename';
import { renderBodyHtml } from './html';
import { defaultHref } from './prepare';
import { estimateReading } from './reading';
import { collapseWs, coverLines, sourceNoteText } from './util';

const BLOCK = new Set([
  'p', 'div', 'section', 'ul', 'ol', 'li', 'h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'blockquote',
  'pre', 'table', 'thead', 'tbody', 'tfoot', 'tr', 'caption', 'dl', 'dt', 'dd', 'hr', 'main',
  'header', 'footer', 'article', 'figure',
]);

interface TxtOpts {
  keepLinks: boolean;
  includeImages: boolean;
}

function textLength(s: string): number {
  return Array.from(s).length;
}

/** Collapses spaces in each line and trims. Keeps the line breaks from <br>. */
function tidy(s: string): string {
  return s
    .split('\n')
    .map((l) => l.replace(/[ \t\f\v\r\u00a0]+/g, ' ').trim())
    .join('\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function inline(node: Node, o: TxtOpts): string {
  if (node.nodeType === 3) return (node.nodeValue ?? '').replace(/\s+/g, ' ');
  if (node.nodeType !== 1) return '';
  const e = node as Element;
  const tag = e.localName.toLowerCase();
  if (tag === 'br') return '\n';
  if (tag === 'img') {
    const alt = collapseWs(e.getAttribute('alt') ?? '');
    if (!o.includeImages && !alt) return '';
    return alt ? ` [Image: ${alt}] ` : ' [Image] ';
  }
  const inner = Array.from(e.childNodes, (c) => inline(c, o)).join('');
  if (tag === 'a') {
    const href = (e.getAttribute('href') ?? '').trim();
    const text = collapseWs(inner);
    if (o.keepLinks && /^(https?:|mailto:)/i.test(href) && text !== href && text !== href.replace(/^mailto:/i, '')) {
      return text ? `${inner} (${href})` : href;
    }
    return inner;
  }
  return inner;
}

function isBlock(n: Node): boolean {
  return n.nodeType === 1 && BLOCK.has((n as Element).localName.toLowerCase());
}

/** Renders the children of an element as a list of text blocks. */
function blocks(el: Element, o: TxtOpts): string[] {
  const out: string[] = [];
  let buf = '';
  const flush = () => {
    const t = tidy(buf);
    if (t) out.push(t);
    buf = '';
  };
  for (const c of Array.from(el.childNodes)) {
    if (isBlock(c)) {
      flush();
      out.push(...block(c as Element, o));
    } else {
      buf += inline(c, o);
    }
  }
  flush();
  return out;
}

function prefixLines(text: string, first: string, rest: string): string {
  return text
    .split('\n')
    .map((l, i) => (i === 0 ? first : l ? rest : rest.trimEnd()) + l)
    .join('\n');
}

function listText(list: Element, o: TxtOpts): string {
  const ordered = list.localName.toLowerCase() === 'ol';
  const startAttr = Number(list.getAttribute('start'));
  let n = Number.isFinite(startAttr) && startAttr > 0 ? startAttr : 1;
  const items: string[] = [];
  for (const li of Array.from(list.children)) {
    const content = li.localName.toLowerCase() === 'li' ? blocks(li, o).join('\n') : block(li, o).join('\n');
    if (!content) continue;
    const marker = ordered ? `${n++}. ` : '- ';
    items.push(prefixLines(content, marker, ' '.repeat(marker.length)));
  }
  return items.join('\n');
}

function block(e: Element, o: TxtOpts): string[] {
  const tag = e.localName.toLowerCase();
  switch (tag) {
    case 'h1':
    case 'h2':
    case 'h3':
    case 'h4':
    case 'h5':
    case 'h6': {
      const t = collapseWs(blocks(e, o).join(' '));
      if (!t) return [];
      return [`${t}\n${(tag === 'h1' ? '=' : '-').repeat(Math.max(3, textLength(t)))}`];
    }
    case 'ul':
    case 'ol': {
      const t = listText(e, o);
      return t ? [t] : [];
    }
    case 'blockquote': {
      const inner = blocks(e, o).join('\n\n');
      return inner ? [prefixLines(inner, '> ', '> ').replace(/^> $/gm, '>')] : [];
    }
    case 'pre': {
      const raw = (e.textContent ?? '').replace(/\r\n?/g, '\n').replace(/^\n+|\s+$/g, '');
      return raw ? [raw.split('\n').map((l) => `    ${l}`).join('\n')] : [];
    }
    case 'hr':
      return ['* * *'];
    case 'table': {
      const lines: string[] = [];
      const cap = e.querySelector('caption');
      if (cap) {
        const c = tidy(inline(cap, o));
        if (c) lines.push(c);
      }
      e.querySelectorAll('tr').forEach((tr) => {
        const cells = Array.from(tr.children)
          .map((td) => tidy(Array.from(td.childNodes, (c) => inline(c, o)).join(' ')).replace(/\n/g, ' '))
          .filter((c) => c !== '');
        if (cells.length) lines.push(cells.join(' | '));
      });
      return lines.length ? [lines.join('\n')] : [];
    }
    case 'dl': {
      const lines: string[] = [];
      for (const c of Array.from(e.children)) {
        const t = blocks(c, o).join('\n');
        if (!t) continue;
        lines.push(c.localName.toLowerCase() === 'dd' ? prefixLines(t, '  ', '  ') : t);
      }
      return lines.length ? [lines.join('\n')] : [];
    }
    default:
      return blocks(e, o);
  }
}

/** Converts an HTML element to readable plain text. */
export function htmlToText(root: Element, o: TxtOpts): string {
  return blocks(root, o).join('\n\n');
}

export function buildTxt(article: Article, opts: BuildOptions): OutputFile {
  const o: TxtOpts = { keepLinks: opts.style.keepLinks, includeImages: opts.includeImages };
  const title = collapseWs(article.title ?? '') || 'Untitled';
  const parts: string[] = [`${title}\n${'='.repeat(Math.max(3, textLength(title)))}`];
  if (opts.includeCover) {
    const lines = coverLines(article).map((l) => `${l.label}: ${l.value}`);
    if (lines.length) parts.push(lines.join('\n'));
  }
  // Keep every <img> as an element, so the text shows "[Image: alt]".
  const container = renderBodyHtml(article, opts, { image: (src) => src || 'image', href: defaultHref });
  const body = htmlToText(container, o);
  if (body) parts.push(body);
  if (opts.includeSourceNote) parts.push(`* * *\n\n${sourceNoteText(article)}`);
  const text = `${parts.join('\n\n')}\n`;
  return {
    name: `${sanitizeFilename(article.title)}.txt`,
    mime: 'text/plain;charset=utf-8',
    blob: new Blob([text], { type: 'text/plain;charset=utf-8' }),
    format: 'txt',
    estimatedPages: estimateReading(article, opts.style).pages,
  };
}
