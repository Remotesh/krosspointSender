import { describe, expect, it } from 'vitest';
import { buildHtml, buildMarkdown, buildTxt } from '../../src/converters';
import { blobText, buildOpts, sampleArticle } from './sample';

describe('buildTxt', () => {
  it('writes readable plain text', async () => {
    const f = buildTxt(sampleArticle(), buildOpts());
    expect(f.format).toBe('txt');
    expect(f.name).toBe('Tea & Coffee a history.txt');
    expect(f.estimatedPages).toBeGreaterThan(0);
    const t = await blobText(f.blob);
    expect(t.startsWith('Tea & <Coffee>: a "history"\n====')).toBe(true);
    expect(t).toContain('Author: Ada Lovelace');
    expect(t).toContain('Tea\n---');
    expect(t).toContain('Coffee\n------');
    expect(t).toContain('a link (https://example.org/ref)');
    expect(t).toContain('Tea is old.\nVery old.');
    expect(t).toContain('[Image: A cup of tea]');
    expect(t).toContain('- Green\n- Black\n  - Assam');
    expect(t).toContain('> Quote here.');
    expect(t).toContain('Drink | Year\nTea | 200');
    expect(t).toContain('    brew --hot');
    expect(t).toContain('Source: https://example.com/articles/tea?x=1&y=2, captured 2026-10-10');
    // Paragraphs are separated by a blank line.
    expect(t).toMatch(/Coffee came later\.\n\n/);
    expect(t).not.toMatch(/<\/?(p|a|b|li|ul|h2|img)/);
  });
  it('drops link URLs when keepLinks is off', async () => {
    const t = await blobText(buildTxt(sampleArticle(), buildOpts({}, { keepLinks: false })).blob);
    expect(t).toContain('a link and');
    expect(t).not.toContain('https://example.org/ref');
  });
  it('does not wrap long lines', async () => {
    const long = 'word '.repeat(60).trim();
    const t = await blobText(buildTxt(sampleArticle({ bodyHtml: `<p>${long}</p>` }), buildOpts()).blob);
    expect(t).toContain(long);
  });
});

describe('buildMarkdown', () => {
  it('uses ATX headings and fenced code', async () => {
    const f = buildMarkdown(sampleArticle(), buildOpts());
    expect(f.format).toBe('md');
    expect(f.name.endsWith('.md')).toBe(true);
    const md = await blobText(f.blob);
    expect(md.startsWith('# Tea & <Coffee>: a "history"')).toBe(true);
    expect(md).toContain('## Tea');
    expect(md).toContain('### Brewing');
    expect(md).toContain('```\nbrew --hot\n```');
    expect(md).toContain('[a link](https://example.org/ref)');
    expect(md).toContain('**bold**');
    expect(md).toContain('![A cup of tea](https://example.com/tea.png)');
    expect(md).toContain('| Drink | Year |');
    expect(md).toMatch(/^-\s+Green$/m);
    expect(md).toContain('> Quote here.');
    expect(md).toContain('Source: https://example.com/articles/tea');
  });
  it('replaces images with alt text when images are off', async () => {
    const md = await blobText(buildMarkdown(sampleArticle(), buildOpts({ includeImages: false })).blob);
    expect(md).not.toContain('![');
    expect(md).toContain('[Image: A cup of tea]');
  });
});

describe('buildHtml', () => {
  it('writes a standalone HTML file with inline CSS', async () => {
    const f = buildHtml(sampleArticle(), buildOpts());
    expect(f.format).toBe('html');
    const html = await blobText(f.blob);
    expect(html.startsWith('<!DOCTYPE html>')).toBe(true);
    expect(html).toContain('<style>');
    expect(html).toContain('img{max-width:100%;height:auto}');
    expect(html).toContain('class="cover"');
    expect(html).toContain('class="source-note"');
    const doc = new DOMParser().parseFromString(html, 'text/html');
    expect(doc.querySelector('h1.title')?.textContent).toBe('Tea & <Coffee>: a "history"');
    expect(doc.querySelectorAll('script').length).toBe(0);
  });
  it('escapes a closing style tag in custom CSS', async () => {
    const html = await blobText(buildHtml(sampleArticle(), buildOpts({}, { customCss: 'p{}</style><script>x</script>' })).blob);
    const doc = new DOMParser().parseFromString(html, 'text/html');
    expect(doc.querySelectorAll('script').length).toBe(0);
  });
});
