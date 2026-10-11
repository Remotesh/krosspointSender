/** Standalone HTML output. It uses the same CSS as the EPUB, inline. */
import type { Article, BuildOptions, OutputFile } from '../types';
import { epubCss } from '../epub/css';
import { sanitizeFilename } from './filename';
import {
  appendSourceNote,
  appendTitleBlock,
  copyNodes,
  defaultHref,
  prepareBody,
  xel,
  type CopyContext,
} from './prepare';
import { estimateReading } from './reading';
import { collapseWs, coverLines, langOf, sourceNoteText } from './util';

/** Extra rules for a desktop browser window. The EPUB does not get these. */
const SCREEN_CSS = `
@media screen and (min-width: 40em) {
  body { max-width: 36em; margin-left: auto; margin-right: auto; padding: 1em; }
}
`;

/** Copies the prepared body into a parent element. Remote image URLs stay. */
export function renderBodyInto(article: Article, opts: BuildOptions, parent: Element, ctx?: CopyContext): void {
  const prepared = prepareBody(article, opts.style);
  copyNodes(prepared.root.childNodes, parent, ctx ?? {
    // The HTML file keeps the original image URLs. data: URLs stay too.
    image: (src) => (opts.includeImages && /^(https?:|data:image\/)/i.test(src) ? src : null),
    href: defaultHref,
  });
}

/** Returns the body content in a container element of a new HTML document. */
export function renderBodyHtml(article: Article, opts: BuildOptions, ctx?: CopyContext): Element {
  const doc = document.implementation.createHTMLDocument('');
  const container = doc.createElement('div');
  doc.body.appendChild(container);
  renderBodyInto(article, opts, container, ctx);
  return container;
}

export function buildHtml(article: Article, opts: BuildOptions): OutputFile {
  const title = collapseWs(article.title ?? '') || 'Untitled';
  const lang = langOf(article);
  const doc = document.implementation.createHTMLDocument(title);
  doc.documentElement.setAttribute('lang', lang);
  const head = doc.head;
  const charset = doc.createElement('meta');
  charset.setAttribute('charset', 'utf-8');
  head.insertBefore(charset, head.firstChild);
  const vp = doc.createElement('meta');
  vp.setAttribute('name', 'viewport');
  vp.setAttribute('content', 'width=device-width, initial-scale=1');
  head.appendChild(vp);
  const gen = doc.createElement('meta');
  gen.setAttribute('name', 'generator');
  gen.setAttribute('content', 'k-rosspointSender');
  head.appendChild(gen);
  const style = doc.createElement('style');
  // A "</style" inside custom CSS would end the element. Escape it.
  style.textContent = (epubCss(opts.style) + SCREEN_CSS).replace(/<\/(style)/gi, '<\\/$1');
  head.appendChild(style);

  const body = doc.body;
  if (opts.includeCover) {
    const cover = xel(doc, 'header', 'cover');
    appendTitleBlock(cover, title, coverLines(article));
    body.appendChild(cover);
    body.appendChild(xel(doc, 'hr'));
  } else {
    appendTitleBlock(body, title, []);
  }
  const main = xel(doc, 'main');
  renderBodyInto(article, opts, main);
  body.appendChild(main);
  if (opts.includeSourceNote) appendSourceNote(body, sourceNoteText(article));

  const html = `<!DOCTYPE html>\n${doc.documentElement.outerHTML}\n`;
  return {
    name: `${sanitizeFilename(article.title)}.html`,
    mime: 'text/html;charset=utf-8',
    blob: new Blob([html], { type: 'text/html;charset=utf-8' }),
    format: 'html',
    estimatedPages: estimateReading(article, opts.style).pages,
  };
}
