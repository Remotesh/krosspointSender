import { describe, expect, it } from 'vitest';
import blogHtml from '../fixtures/article-blog.html?raw';
import wikiHtml from '../fixtures/wikipedia-sample.html?raw';
import sampleMd from '../fixtures/sample.md?raw';
import sampleTxt from '../fixtures/sample.txt?raw';
import { extractArticle, extractWikipedia } from '../../src/extractors';

const FIXTURES: Record<string, string> = {
  'article-blog.html': blogHtml,
  'wikipedia-sample.html': wikiHtml,
  'sample.md': sampleMd,
  'sample.txt': sampleTxt,
};
const fixture = (name: string): string => FIXTURES[name];

describe('extractArticle on a blog post', async () => {
  const html = fixture('article-blog.html');
  const a = await extractArticle({ kind: 'url', content: html, sourceUrl: 'https://crumbjournal.example/posts/slow-bread' });

  it('reads the metadata', () => {
    expect(a.title).toBe('Why Slow Bread Tastes Better');
    expect(a.author).toBe('Ada Baker');
    expect(a.publishedAt).toBe('2025-03-14T09:30:00.000Z');
    expect(a.siteName).toBe('The Crumb Journal');
    expect(a.excerpt).toMatch(/long fermentation/);
    expect(a.language).toBe('en-GB');
    expect(a.inputKind).toBe('url');
    expect(a.sourceUrl).toBe('https://crumbjournal.example/posts/slow-bread');
    expect(a.id).toMatch(/^[0-9a-f-]{36}$/);
    expect(Number.isNaN(Date.parse(a.capturedAt))).toBe(false);
  });

  it('keeps the article text', () => {
    expect(a.bodyHtml).toContain('Most home bakers rush the dough');
    expect(a.bodyHtml).toContain('Short rises do not give the enzymes');
    expect(a.bodyHtml).toContain('Time is the cheapest ingredient');
    expect(a.bodyHtml).toMatch(/<pre><code>hydration/);
    expect(a.bodyHtml).toMatch(/<ol>[\s\S]*<li>Mix at 6 pm\.<\/li>/);
    expect(a.bodyHtml).toMatch(/<table>[\s\S]*Cold proof/);
    expect(a.wordCount).toBeGreaterThan(200);
  });

  it('removes junk', () => {
    for (const junk of ['cookies', 'ADVERTISEMENT', 'Share on', 'Related posts', 'casino', 'All rights reserved', 'Subscribe', 'Ten tips', 'Recipes']) {
      expect(a.bodyHtml).not.toContain(junk);
    }
    expect(a.bodyHtml).not.toMatch(/<script|<style|<iframe|<form|onclick|style=|class=/);
    expect(a.bodyHtml).not.toContain('javascript:');
  });

  it('resolves links and images, and builds figures', () => {
    expect(a.bodyHtml).toContain('href="https://crumbjournal.example/glossary/long-fermentation"');
    expect(a.images).toEqual([
      { src: 'https://crumbjournal.example/images/crumb-480.jpg', alt: 'Open crumb of a slow-fermented loaf' },
      { src: 'https://cdn.crumbjournal.example/img/fridge.jpg', alt: 'Loaves in a fridge' },
    ]);
    expect(a.bodyHtml).toContain(
      '<figure><img src="https://crumbjournal.example/images/crumb-480.jpg" alt="Open crumb of a slow-fermented loaf"><figcaption>An open crumb after an 18-hour cold rise.</figcaption></figure>',
    );
    expect(a.bodyHtml).toMatch(/<figure><img src="https:\/\/cdn\.crumbjournal\.example\/img\/fridge\.jpg"[^>]*><figcaption>Shaped loaves/);
  });

  it('builds the TOC with sec-N ids on every heading', () => {
    expect(a.toc.map((t) => t.text)).toEqual(['What happens during a slow rise', 'Using the fridge', 'Timing table', 'A simple schedule']);
    expect(a.toc.map((t) => t.id)).toEqual(['sec-1', 'sec-2', 'sec-3', 'sec-4']);
    const doc = new DOMParser().parseFromString(`<body>${a.bodyHtml}</body>`, 'text/html');
    for (const h of Array.from(doc.querySelectorAll('h1,h2,h3,h4,h5,h6'))) expect(h.id).toMatch(/^sec-\d+$/);
  });

  it('applies keepLinks and keepImages', async () => {
    const b = await extractArticle({ kind: 'html', content: html }, { keepLinks: false, keepImages: false });
    expect(b.bodyHtml).not.toContain('<a ');
    expect(b.bodyHtml).not.toContain('<img');
    expect(b.images).toEqual([]);
    expect(b.inputKind).toBe('html');
  });
});

describe('extractWikipedia', () => {
  const html = fixture('wikipedia-sample.html');
  const url = 'https://en.wikipedia.org/wiki/Sourdough';

  it('cleans the article by default', () => {
    const a = extractWikipedia(html, url);
    expect(a.title).toBe('Sourdough');
    expect(a.author).toBe('Wikipedia contributors');
    expect(a.siteName).toBe('Wikipedia');
    expect(a.language).toBe('en');
    expect(a.inputKind).toBe('wikipedia');
    expect(a.bodyHtml).toContain('is a type of');
    expect(a.bodyHtml).toContain('href="https://en.wikipedia.org/wiki/Bread"');
    // Removed parts.
    for (const gone of ['InfoboxTypeBread', 'ReferenceOneText', 'SeeAlsoRyeBread', 'ExternalLinkSite', 'NavboxBreads', 'Contents', 'Categories', 'Jump to content', 'needs additional citations', 'For the gold rush miners', 'Bread made by fermentation', 'From Wikipedia', 'last edited', '[1]', '[edit]', '>edit<']) {
      expect(a.bodyHtml).not.toContain(gone);
    }
    expect(a.toc.map((t) => t.text)).toEqual(['History', 'Gold rush', 'Chemistry']);
  });

  it('converts thumbs to figures with https images and prefers the srcset 2x image', () => {
    const a = extractWikipedia(html, url);
    expect(a.bodyHtml).toContain(
      '<figure><img src="https://upload.wikimedia.org/wikipedia/commons/thumb/b/b2/Boulangerie.jpg/440px-Boulangerie.jpg" alt="A bakery display"><figcaption>Sourdough loaves in a French <a href="https://en.wikipedia.org/wiki/Boulangerie">boulangerie</a></figcaption></figure>',
    );
    expect(a.bodyHtml).toContain('<figcaption>A prospector in 1898</figcaption>');
    expect(a.bodyHtml).not.toContain('srcset');
    expect(a.images.map((i) => i.src)).toEqual([
      'https://upload.wikimedia.org/wikipedia/commons/thumb/b/b2/Boulangerie.jpg/440px-Boulangerie.jpg',
      'https://upload.wikimedia.org/wikipedia/commons/thumb/c/c3/Miner.jpg/220px-Miner.jpg',
    ]);
  });

  it('keeps the wikitable', () => {
    const a = extractWikipedia(html, url);
    expect(a.bodyHtml).toMatch(/<table><caption>Typical starter composition<\/caption>/);
  });

  it('keeps the infobox and references when opted in', () => {
    const a = extractWikipedia(html, url, { keepInfobox: true, keepReferences: true });
    expect(a.bodyHtml).toContain('InfoboxTypeBread');
    expect(a.bodyHtml).toContain('ReferenceOneText');
    expect(a.toc.map((t) => t.text)).toContain('References');
    // These sections go even with keepReferences.
    expect(a.bodyHtml).not.toContain('SeeAlsoRyeBread');
    expect(a.bodyHtml).not.toContain('ExternalLinkSite');
  });

  it('is used by extractArticle for kind wikipedia', async () => {
    const a = await extractArticle({ kind: 'wikipedia', content: html, sourceUrl: url });
    expect(a.title).toBe('Sourdough');
    expect(a.inputKind).toBe('wikipedia');
  });
});

describe('extractArticle for text, markdown and bookmarklet', () => {
  it('handles Markdown', async () => {
    const a = await extractArticle({ kind: 'markdown', content: fixture('sample.md'), fileName: 'sample.md' });
    expect(a.title).toBe('Field Notes on E-Ink Reading');
    expect(a.bodyHtml).toContain('<strong>easy on the eyes</strong>');
    expect(a.bodyHtml).toContain('<pre><code>');
    expect(a.bodyHtml).not.toContain('script');
    expect(a.toc[0]).toEqual({ id: 'sec-1', level: 1, text: 'Field Notes on E-Ink Reading' });
  });

  it('handles plain text', async () => {
    const a = await extractArticle({ kind: 'text', content: fixture('sample.txt'), fileName: 'paper.txt' });
    expect(a.title).toBe('A Short Note on Paper');
    expect(a.bodyHtml).toContain('&lt;b&gt;tags&lt;/b&gt;');
    expect(a.bodyHtml).toContain('Roses are red,<br>violets are blue,');
    expect(a.bodyHtml).toMatch(/trade routes to the Middle East/);
    expect(a.wordCount).toBeGreaterThan(40);
  });

  it('falls back to the file name for an empty title', async () => {
    const a = await extractArticle({ kind: 'text', content: '', fileName: 'my_notes.txt' });
    expect(a.title).toBe('my notes');
  });

  it('handles a bookmarklet payload', async () => {
    const payload = {
      v: 1,
      url: 'https://crumbjournal.example/posts/slow-bread',
      title: 'Captured title',
      html: fixture('article-blog.html'),
      capturedAt: '2026-10-01T12:00:00.000Z',
    };
    const a = await extractArticle({ kind: 'bookmarklet', content: JSON.stringify(payload) });
    expect(a.inputKind).toBe('bookmarklet');
    expect(a.capturedAt).toBe('2026-10-01T12:00:00.000Z');
    expect(a.sourceUrl).toBe(payload.url);
    expect(a.bodyHtml).toContain('Most home bakers rush the dough');
  });

  it('throws for an empty URL fetch', async () => {
    await expect(extractArticle({ kind: 'url', content: '' })).rejects.toThrow();
  });
});
