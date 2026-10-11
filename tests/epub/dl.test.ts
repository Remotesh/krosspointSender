import JSZip from 'jszip';
import { describe, expect, it } from 'vitest';
import { buildEpub } from '../../src/epub';
import { blobBytes, buildOpts, sampleArticle } from '../converters/sample';

const XHTML = 'application/xhtml+xml';

/** Wikipedia-style definition lists, with and without <dd>. */
const BODY = [
  '<p>Intro.</p>',
  '<dl><dt>Pseudo heading</dt></dl>',
  '<p>Text under it.</p>',
  '<dl><dt>Term A</dt><dd>Def A</dd><dt>Orphan term</dt></dl>',
  '<dl><dd>Indented quote</dd></dl>',
  '<dl><dt>Two</dt><dt>Terms</dt><dd>One def</dd><dd>Two defs</dd></dl>',
  '<dl><dt>Outer</dt><dd><dl><dt>Inner heading</dt></dl></dd></dl>',
].join('');

async function chapterDoc(): Promise<Document> {
  const f = await buildEpub(sampleArticle({ bodyHtml: BODY, toc: [], images: [] }), buildOpts({ includeCover: false }));
  const zip = await JSZip.loadAsync(await blobBytes(f.blob));
  const src = await zip.file('OEBPS/chapter-001.xhtml')!.async('string');
  const doc = new DOMParser().parseFromString(src, XHTML);
  expect(doc.getElementsByTagName('parsererror').length).toBe(0);
  return doc;
}

describe('definition lists', () => {
  it('makes every dl valid (dt+ dd+ groups) and turns lone dt into bold paragraphs', async () => {
    const doc = await chapterDoc();
    for (const dl of Array.from(doc.getElementsByTagName('dl'))) {
      const tags = Array.from(dl.children).map((c) => c.localName);
      expect(tags, dl.outerHTML).toContain('dd');
      expect(tags[0], dl.outerHTML).toBe('dt');
      expect(tags[tags.length - 1], dl.outerHTML).toBe('dd');
    }
    const bold = Array.from(doc.querySelectorAll('p > b')).map((b) => b.textContent);
    expect(bold).toEqual(expect.arrayContaining(['Pseudo heading', 'Orphan term', 'Inner heading']));
    // The pseudo heading sits before the text that follows it.
    const ps = Array.from(doc.getElementsByTagName('p')).map((p) => p.textContent);
    expect(ps.indexOf('Pseudo heading')).toBeLessThan(ps.indexOf('Text under it.'));
    expect(ps.indexOf('Orphan term')).toBeGreaterThan(-1);
    expect(doc.body.textContent).toContain('Indented quote');
    expect(doc.body.textContent).toContain('Two defs');
  });
});
