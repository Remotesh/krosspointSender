/** Shared test data for the converter and EPUB tests. */
import type { Article, BuildOptions, StyleSettings } from '../../src/types';
import { PRESETS } from '../../src/epub';

/** A 1x1 transparent PNG. */
export const PNG_1X1 = Uint8Array.from(
  atob('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=='),
  (c) => c.charCodeAt(0),
);

/** A tiny GIF (1x1). */
export const GIF_1X1 = Uint8Array.from(
  atob('R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7'),
  (c) => c.charCodeAt(0),
);

export function sampleArticle(over: Partial<Article> = {}): Article {
  return {
    id: '3b241101-e2bb-4255-8caf-4136c566a962',
    title: 'Tea & <Coffee>: a "history"',
    author: 'Ada Lovelace',
    publishedAt: '2024-05-01T10:00:00Z',
    capturedAt: '2026-10-10T08:30:00Z',
    sourceUrl: 'https://example.com/articles/tea?x=1&y=2',
    siteName: 'Example Times',
    excerpt: 'A short history of hot drinks.',
    language: 'en',
    tags: ['history', 'drinks & food'],
    bodyHtml: [
      '<div><div>',
      '<p>Intro paragraph with <a href="https://example.org/ref">a link</a> and <b>bold</b>.</p>',
      '<h2 id="sec-1">Tea</h2>',
      '<p>Tea is old.<br>Very old.</p>',
      '<figure><img src="https://example.com/tea.png" alt="A cup of tea"><figcaption>Fig. 1</figcaption></figure>',
      '<ul><li>Green</li><li>Black<ul><li>Assam</li></ul></li></ul>',
      '<h3 id="sec-2">Brewing</h3>',
      '<p>See <a href="#sec-3">coffee</a>.</p>',
      '<h2 id="sec-3">Coffee</h2>',
      '<p>Coffee came later.</p>',
      '<img src="https://example.com/coffee.gif" alt="Coffee beans">',
      '<img src="https://example.com/missing.jpg" alt="Missing image">',
      '<blockquote><p>Quote here.</p></blockquote>',
      '<table><tr><th>Drink</th><th>Year</th></tr><tr><td>Tea</td><td>200</td></tr></table>',
      '<pre><code>brew --hot</code></pre>',
      '</div></div>',
    ].join(''),
    toc: [
      { id: 'sec-1', level: 2, text: 'Tea' },
      { id: 'sec-2', level: 3, text: 'Brewing' },
      { id: 'sec-3', level: 2, text: 'Coffee' },
    ],
    images: [
      { src: 'https://example.com/tea.png', alt: 'A cup of tea' },
      { src: 'https://example.com/coffee.gif', alt: 'Coffee beans' },
      { src: 'https://example.com/missing.jpg', alt: 'Missing image' },
    ],
    wordCount: 400,
    inputKind: 'url',
    ...over,
  };
}

export function style(over: Partial<StyleSettings> = {}): StyleSettings {
  return { ...PRESETS.comfortable, ...over };
}

export function buildOpts(over: Partial<BuildOptions> = {}, s: Partial<StyleSettings> = {}): BuildOptions {
  return {
    style: style(s),
    includeImages: true,
    includeCover: true,
    includeSourceNote: true,
    ...over,
  };
}

export function sampleImages(): Map<string, Blob> {
  return new Map<string, Blob>([
    ['https://example.com/tea.png', new Blob([PNG_1X1], { type: 'image/png' })],
    ['https://example.com/coffee.gif', new Blob([GIF_1X1], { type: 'image/gif' })],
  ]);
}

/** Reads a Blob in jsdom, with or without Blob.arrayBuffer. */
export async function blobBytes(blob: Blob): Promise<Uint8Array> {
  if (typeof blob.arrayBuffer === 'function') return new Uint8Array(await blob.arrayBuffer());
  return new Promise((resolve, reject) => {
    const r = new FileReader();
    r.onload = () => resolve(new Uint8Array(r.result as ArrayBuffer));
    r.onerror = () => reject(r.error);
    r.readAsArrayBuffer(blob);
  });
}

export async function blobText(blob: Blob): Promise<string> {
  return new TextDecoder().decode(await blobBytes(blob));
}
