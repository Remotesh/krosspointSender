import JSZip from 'jszip';
import { describe, expect, it } from 'vitest';
import { buildBundleEpub, buildEpub, epubCss, PRESETS, sendTestEpub } from '../../src/epub';
import { blobBytes, buildOpts, sampleArticle, sampleImages } from '../converters/sample';

const XHTML = 'application/xhtml+xml';

async function open(blob: Blob): Promise<{ zip: JSZip; bytes: Uint8Array }> {
  const bytes = await blobBytes(blob);
  const zip = await JSZip.loadAsync(bytes);
  return { zip, bytes };
}

function parseXml(text: string, type: DOMParserSupportedType = 'application/xml'): Document {
  const doc = new DOMParser().parseFromString(text, type);
  expect(doc.getElementsByTagName('parsererror').length, text.slice(0, 300)).toBe(0);
  return doc;
}

async function text(zip: JSZip, path: string): Promise<string> {
  const f = zip.file(path);
  expect(f, `missing ${path}`).not.toBeNull();
  return f!.async('string');
}

async function checkStructure(blob: Blob): Promise<{ zip: JSZip; opf: Document }> {
  const { zip, bytes } = await open(blob);
  // The first entry is "mimetype", stored without compression.
  expect(Object.keys(zip.files)[0]).toBe('mimetype');
  expect(new TextDecoder().decode(bytes.subarray(30, 38))).toBe('mimetype');
  expect(bytes[8] | (bytes[9] << 8)).toBe(0);
  // No extra field, so the content follows the name at byte 38.
  expect(bytes[28] | (bytes[29] << 8)).toBe(0);
  expect(new TextDecoder().decode(bytes.subarray(38, 58))).toBe('application/epub+zip');
  expect(await text(zip, 'mimetype')).toBe('application/epub+zip');

  const container = parseXml(await text(zip, 'META-INF/container.xml'));
  const rootfile = container.getElementsByTagName('rootfile')[0];
  expect(rootfile.getAttribute('full-path')).toBe('OEBPS/content.opf');

  const opf = parseXml(await text(zip, 'OEBPS/content.opf'));
  expect(opf.documentElement.getAttribute('version')).toBe('3.0');
  expect(zip.file('OEBPS/nav.xhtml')).not.toBeNull();
  expect(zip.file('OEBPS/toc.ncx')).not.toBeNull();
  expect(zip.file('OEBPS/style.css')).not.toBeNull();
  parseXml(await text(zip, 'OEBPS/toc.ncx'));
  const nav = parseXml(await text(zip, 'OEBPS/nav.xhtml'), XHTML);
  expect(nav.getElementsByTagName('nav')[0].getAttributeNS('http://www.idpf.org/2007/ops', 'type')).toBe('toc');

  // Every manifest item exists in the zip.
  for (const item of Array.from(opf.getElementsByTagName('item'))) {
    expect(zip.file(`OEBPS/${item.getAttribute('href')}`), item.getAttribute('href') ?? '').not.toBeNull();
  }
  // Every XHTML file parses and has no remote image.
  for (const name of Object.keys(zip.files).filter((n) => n.endsWith('.xhtml'))) {
    const src = await text(zip, name);
    parseXml(src, XHTML);
    expect(src).not.toMatch(/<img[^>]+src="http/);
  }
  return { zip, opf };
}

function dc(opf: Document, name: string): string[] {
  return Array.from(opf.getElementsByTagNameNS('http://purl.org/dc/elements/1.1/', name)).map((e) => e.textContent ?? '');
}

describe('buildEpub', () => {
  it('writes a valid EPUB 3 structure with metadata', async () => {
    const f = await buildEpub(sampleArticle(), buildOpts(), sampleImages());
    expect(f.format).toBe('epub');
    expect(f.mime).toBe('application/epub+zip');
    expect(f.name).toBe('Tea & Coffee a history.epub');
    expect(f.estimatedPages).toBeGreaterThan(0);
    const { zip, opf } = await checkStructure(f.blob);
    expect(dc(opf, 'title')).toEqual(['Tea & <Coffee>: a "history"']);
    expect(dc(opf, 'creator')).toEqual(['Ada Lovelace']);
    expect(dc(opf, 'source')).toEqual(['https://example.com/articles/tea?x=1&y=2']);
    expect(dc(opf, 'identifier')[0]).toBe('urn:uuid:3b241101-e2bb-4255-8caf-4136c566a962');
    expect(dc(opf, 'language')).toEqual(['en']);
    expect(dc(opf, 'date')).toEqual(['2024-05-01']);
    expect(dc(opf, 'subject')).toEqual(['history', 'drinks & food']);
    const modified = Array.from(opf.getElementsByTagName('meta')).find((m) => m.getAttribute('property') === 'dcterms:modified');
    expect(modified?.textContent).toMatch(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/);
    expect(zip.file('OEBPS/cover.xhtml')).not.toBeNull();
  });

  it('splits at h1/h2 and links nav and NCX to the right files', async () => {
    const f = await buildEpub(sampleArticle(), buildOpts(), sampleImages());
    const { zip, opf } = await checkStructure(f.blob);
    const chapters = Object.keys(zip.files).filter((n) => /OEBPS\/chapter-\d{3}\.xhtml$/.test(n)).sort();
    // Intro, Tea, Coffee.
    expect(chapters).toEqual(['OEBPS/chapter-001.xhtml', 'OEBPS/chapter-002.xhtml', 'OEBPS/chapter-003.xhtml']);
    const spine = Array.from(opf.getElementsByTagName('itemref')).map((i) => i.getAttribute('idref'));
    expect(spine).toEqual(['cover', 'chapter-001', 'chapter-002', 'chapter-003']);

    const nav = await text(zip, 'OEBPS/nav.xhtml');
    expect(nav).toContain('href="chapter-002.xhtml"');
    expect(nav).toContain('href="chapter-002.xhtml#sec-2"');
    expect(nav).toContain('href="chapter-003.xhtml"');
    const ncx = await text(zip, 'OEBPS/toc.ncx');
    expect(ncx).toContain('src="chapter-002.xhtml#sec-2"');
    expect(ncx).toContain('<text>Brewing</text>');
    // The internal link in chapter 2 points to chapter 3.
    const ch2 = await text(zip, 'OEBPS/chapter-002.xhtml');
    expect(ch2).toContain('href="chapter-003.xhtml#sec-3"');
    // The source note is the last section of the last chapter.
    const ch3 = parseXml(await text(zip, 'OEBPS/chapter-003.xhtml'), XHTML);
    const sections = ch3.getElementsByTagName('section');
    const last = sections[sections.length - 1];
    expect(last.getAttribute('class')).toBe('source-note');
    expect(last.textContent).toContain('Source: https://example.com/articles/tea?x=1&y=2, captured 2026-10-10');
    expect(last.textContent).toContain('generated by K-rosspointSender');
  });

  it('writes local images and keeps alt text for dropped ones', async () => {
    const f = await buildEpub(sampleArticle(), buildOpts(), sampleImages());
    const { zip, opf } = await checkStructure(f.blob);
    // The PNG passes through. The GIF cannot be converted without a canvas. The JPEG has no blob.
    expect(zip.file('OEBPS/images/img-001.png')).not.toBeNull();
    expect(zip.file(/images\//).length).toBe(1);
    const item = Array.from(opf.getElementsByTagName('item')).find((i) => i.getAttribute('href') === 'images/img-001.png');
    expect(item?.getAttribute('media-type')).toBe('image/png');
    const ch2 = await text(zip, 'OEBPS/chapter-002.xhtml');
    expect(ch2).toContain('src="images/img-001.png"');
    expect(ch2).toContain('alt="A cup of tea"');
    const ch3 = await text(zip, 'OEBPS/chapter-003.xhtml');
    expect(ch3).toContain('<p class="img-alt">Coffee beans</p>');
    expect(ch3).toContain('<p class="img-alt">Missing image</p>');
  });

  it('drops all images when images are off', async () => {
    const f = await buildEpub(sampleArticle(), buildOpts({ includeImages: false }), sampleImages());
    const { zip } = await checkStructure(f.blob);
    expect(zip.file(/images\//).length).toBe(0);
  });

  it('works without cover and source note, and with one chapter', async () => {
    const a = sampleArticle({ bodyHtml: '<p>Only text &amp; more.</p><h2>One</h2><p>x</p>', images: [] });
    const f = await buildEpub(a, buildOpts({ includeCover: false, includeSourceNote: false }));
    const { zip, opf } = await checkStructure(f.blob);
    expect(zip.file('OEBPS/cover.xhtml')).toBeNull();
    expect(Object.keys(zip.files).filter((n) => n.includes('chapter-'))).toEqual(['OEBPS/chapter-001.xhtml']);
    expect(Array.from(opf.getElementsByTagName('itemref')).map((i) => i.getAttribute('idref'))).toEqual(['chapter-001']);
    const ch = await text(zip, 'OEBPS/chapter-001.xhtml');
    expect(ch).toContain('<h1 class="title">Tea &amp; &lt;Coffee&gt;: a "history"</h1>');
    expect(ch).not.toContain('source-note');
  });

  it('survives hostile input', async () => {
    const a = sampleArticle({
      title: 'Bad \u0001 chars ]]> & stuff',
      bodyHtml:
        '<p>Ctl\u0002 text</p><script>alert(1)</script><p id="1 bad id">x</p><p id="1 bad id">y</p>' +
        '<custom-el foo:bar="1">inner</custom-el><svg><text>no</text></svg><a href="javascript:alert(1)">js</a>',
      images: [],
      tags: ['<tag>'],
    });
    const f = await buildEpub(a, buildOpts());
    const { zip, opf } = await checkStructure(f.blob);
    const ch = await text(zip, 'OEBPS/chapter-001.xhtml');
    expect(ch).not.toContain('script');
    expect(ch).not.toContain('javascript:');
    expect(ch).toContain('inner');
    expect(dc(opf, 'subject')).toEqual(['<tag>']);
    const ids = Array.from(parseXml(ch, XHTML).querySelectorAll('[id]')).map((e) => e.getAttribute('id'));
    expect(new Set(ids).size).toBe(ids.length);
  });
});

describe('buildBundleEpub', () => {
  it('writes one chapter per article with nested nav', async () => {
    const a1 = sampleArticle({ title: 'First' });
    const a2 = sampleArticle({ id: 'x', title: 'Second', bodyHtml: '<h2 id="sec-1">Part</h2><p>Body</p>', images: [] });
    const a3 = sampleArticle({ title: 'Third', bodyHtml: '<p>Third body</p>', images: [] });
    const f = await buildBundleEpub([a1, a2, a3], { title: 'My Bundle', author: 'Me' }, buildOpts(), sampleImages());
    expect(f.name).toBe('My Bundle.epub');
    const { zip, opf } = await checkStructure(f.blob);
    const chapters = Object.keys(zip.files).filter((n) => /chapter-\d{3}\.xhtml$/.test(n)).sort();
    expect(chapters).toEqual(['OEBPS/chapter-001.xhtml', 'OEBPS/chapter-002.xhtml', 'OEBPS/chapter-003.xhtml']);
    expect(dc(opf, 'title')).toEqual(['My Bundle']);
    expect(zip.file('OEBPS/images/img-a01-001.png')).not.toBeNull();
    const cover = await text(zip, 'OEBPS/cover.xhtml');
    expect(cover).toContain('First');
    expect(cover).toContain('Second');
    expect(cover).toContain('Third');
    const nav = parseXml(await text(zip, 'OEBPS/nav.xhtml'), XHTML);
    const top = Array.from(nav.querySelector('nav > ol')!.children).map((li) => li.querySelector('a')!.textContent);
    expect(top).toEqual(['Title page', 'First', 'Second', 'Third']);
    expect(nav.querySelector('a[href="chapter-002.xhtml#sec-1"]')?.textContent).toBe('Part');
    expect(f.estimatedPages).toBeGreaterThanOrEqual(3);
  });
});

describe('sendTestEpub', () => {
  it('writes a tiny valid EPUB', async () => {
    const f = await sendTestEpub();
    expect(f.name).toBe('krosspoint-test.epub');
    const { zip, opf } = await checkStructure(f.blob);
    expect(dc(opf, 'title')).toEqual(['K-rosspointSender test']);
    expect(await text(zip, 'OEBPS/chapter-001.xhtml')).toContain('The transfer works');
  });
});

describe('epubCss and PRESETS', () => {
  it('has the preset values from the spec', () => {
    expect(PRESETS.compact).toMatchObject({ fontSizePercent: 90, marginEm: 0.4, lineHeight: 1.25, paragraphSpacingEm: 0.3 });
    expect(PRESETS.comfortable).toMatchObject({ fontSizePercent: 100, marginEm: 0.8, lineHeight: 1.45, paragraphSpacingEm: 0.6 });
    expect(PRESETS.large).toMatchObject({ fontSizePercent: 125, marginEm: 0.8, lineHeight: 1.5, paragraphSpacingEm: 0.7 });
    expect(PRESETS.reference).toMatchObject({
      fontSizePercent: 95, marginEm: 0.5, lineHeight: 1.35, paragraphSpacingEm: 0.4, tableHandling: 'keep', keepLinks: true,
    });
  });
  it('writes simple CSS', () => {
    const css = epubCss({ ...PRESETS.comfortable, hyphenate: true, customCss: '.x { font-style: italic; }' });
    expect(css).toContain('img{max-width:100%;height:auto}');
    expect(css).toContain('table{max-width:100%;table-layout:fixed}');
    expect(css).toContain('hyphens: auto');
    expect(css).toContain('line-height: 1.45');
    expect(css.trim().endsWith('.x { font-style: italic; }')).toBe(true);
    for (const banned of ['float', 'column', '@font-face', 'animation', 'position']) {
      expect(css.split('/* Custom CSS */')[0]).not.toContain(banned);
    }
    expect(epubCss({ ...PRESETS.comfortable, hyphenate: false })).not.toContain('hyphens: auto');
  });
});

describe('NCX playOrder and nested captions', () => {
  it('gives every navPoint a unique id and a gap-free playOrder', async () => {
    const { buildEpub, PRESETS } = await import('../../src/epub');
    const JSZip = (await import('jszip')).default;
    const article = {
      id: 'ncx-1', title: 'Nested', capturedAt: new Date().toISOString(), tags: [], images: [], wordCount: 20, inputKind: 'html' as const,
      bodyHtml: '<h1 id="s1">A</h1><p>x</p><h2 id="s2">B</h2><h3 id="s3">C</h3><h4 id="s4">D</h4><h1 id="s5">E</h1><h2 id="s6">F</h2><figure><img src="https://x/y.png" alt="a"/><figcaption>Cap <table><tr><td>1</td></tr></table></figcaption></figure>',
      toc: [1, 2, 3, 4, 5, 6].map((i) => ({ id: `s${i}`, level: [1, 2, 3, 4, 1, 2][i - 1], text: 'ABCDEF'[i - 1] })),
    };
    const out = await buildEpub(article, { style: PRESETS.comfortable, includeImages: false, includeCover: true, includeSourceNote: true });
    const zip = await JSZip.loadAsync(await out.blob.arrayBuffer());
    const ncx = await zip.file('OEBPS/toc.ncx')!.async('string');
    const orders = [...ncx.matchAll(/playOrder="(\d+)"/g)].map((m) => Number(m[1])).sort((a, b) => a - b);
    expect(orders).toEqual(orders.map((_, i) => i + 1));
    const ids = [...ncx.matchAll(/id="(np-\d+)"/g)].map((m) => m[1]);
    expect(new Set(ids).size).toBe(ids.length);
    const chapters = await Promise.all(Object.keys(zip.files).filter((n) => /chapter-\d+\.xhtml$/.test(n)).map((n) => zip.file(n)!.async('string')));
    expect(chapters.join('')).not.toMatch(/<p[^>]*>[^<]*<table/);
  });
});
