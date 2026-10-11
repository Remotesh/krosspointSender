import JSZip from 'jszip';
import { describe, expect, it } from 'vitest';
import { buildPackEpub } from '../../src/epub';
import type { Article, Pack, PackPage, PackProgress } from '../../src/types';
import { blobBytes, buildOpts, PNG_1X1 } from '../converters/sample';

const XHTML = 'application/xhtml+xml';
const IMG = 'https://upload.wikimedia.org/steam.png';

function article(title: string, body: string, over: Partial<Article> = {}): Article {
  return {
    id: `id-${title}`,
    title,
    capturedAt: '2026-10-10T08:00:00Z',
    sourceUrl: `https://en.wikipedia.org/wiki/${title.replace(/ /g, '_')}`,
    siteName: 'Wikipedia',
    language: 'en',
    tags: [],
    bodyHtml: body,
    toc: [],
    images: [],
    wordCount: 300,
    inputKind: 'wikipedia',
    ...over,
  };
}

function page(title: string, fileName: string | undefined, body: string, depth = 1): PackPage {
  const a = article(title, body);
  return {
    candidate: { key: title, title, url: a.sourceUrl as string, depth, linkCount: 1, selected: true },
    state: 'done',
    article: a,
    fileName,
  };
}

function samplePack(over: Partial<Pack['limits']> = {}): Pack {
  const pages: PackPage[] = [
    page(
      'Steam engine',
      'steam_engine.xhtml',
      '<p>A steam engine uses a <a href="boiler.xhtml">boiler</a> and a <a href="piston.xhtml#History">piston</a>.</p>' +
        `<figure><img src="${IMG}" alt="Engine"><figcaption>An engine</figcaption></figure>` +
        '<h2 id="Design">Design</h2><p>Invented by <a class="ext" href="https://en.wikipedia.org/wiki/James_Watt">James Watt ↗</a>.' +
        ' See <a href="https://example.org/ref">a reference</a>.</p><h3 id="Valves">Valves</h3><p>Valves move.</p>',
      0,
    ),
    page('Boiler', 'boiler.xhtml', '<p>A boiler heats water for the <a href="steam_engine.xhtml">steam engine</a>.</p><h2 id="Types">Types</h2><p>Fire tube.</p>'),
    page('Piston', 'piston.xhtml', '<p>A piston moves.</p><h2 id="History">History</h2><p>Old.</p>'),
  ];
  pages.push({
    candidate: { key: 'Condenser', title: 'Condenser', url: 'https://en.wikipedia.org/wiki/Condenser', depth: 1, linkCount: 1, selected: true },
    state: 'failed',
    error: 'HTTP 500',
  });
  return {
    id: '7c9e6679-7425-40de-944b-e07fc1f90ae7',
    kind: 'wikipedia',
    title: 'Steam engine pack',
    rootKey: 'Steam engine',
    rootUrl: 'https://en.wikipedia.org/wiki/Steam_engine',
    limits: {
      depth: 1,
      maxArticles: 20,
      maxImagesPerArticle: 2,
      maxTotalEpubSizeMB: 25,
      requestDelayMs: 0,
      includeReferences: false,
      includeInfobox: false,
      sameDomainOnly: true,
      ...over,
    },
    pages,
    externalLinks: [
      { key: 'James Watt', title: 'James Watt', url: 'https://en.wikipedia.org/wiki/James_Watt', fromKey: 'Steam engine' },
    ],
    createdAt: '2026-10-10T08:00:00Z',
    updatedAt: '2026-10-10T08:00:00Z',
  };
}

function parseXml(src: string, type: DOMParserSupportedType = 'application/xml'): Document {
  const doc = new DOMParser().parseFromString(src, type);
  expect(doc.getElementsByTagName('parsererror').length, src.slice(0, 300)).toBe(0);
  return doc;
}

async function text(zip: JSZip, path: string): Promise<string> {
  const f = zip.file(path);
  expect(f, `missing ${path}`).not.toBeNull();
  return f!.async('string');
}

describe('buildPackEpub', () => {
  it('builds a valid pack EPUB with links, nav, NCX and attribution', async () => {
    const events: PackProgress[] = [];
    const images = new Map([[IMG, new Blob([PNG_1X1], { type: 'image/png' })]]);
    const f = await buildPackEpub(samplePack(), buildOpts(), images, (p) => events.push(p));
    expect(f.name).toBe('Steam engine pack.epub');
    expect(f.mime).toBe('application/epub+zip');
    expect(f.estimatedPages).toBe(6); // 3 pages x 300 words at 170 words per page.

    const bytes = await blobBytes(f.blob);
    // Set PACK_EPUB_OUT to a path to keep the EPUB for epubcheck.
    const out = (globalThis as { process?: { env: Record<string, string | undefined> } }).process?.env.PACK_EPUB_OUT;
    if (out) {
      const fsName = 'node:fs';
      const fs = (await import(/* @vite-ignore */ fsName)) as { writeFileSync(p: string, d: Uint8Array): void };
      fs.writeFileSync(out, bytes);
    }
    const zip = await JSZip.loadAsync(bytes);

    // mimetype first and stored.
    expect(Object.keys(zip.files)[0]).toBe('mimetype');
    expect(new TextDecoder().decode(bytes.subarray(30, 38))).toBe('mimetype');
    expect(bytes[8] | (bytes[9] << 8)).toBe(0);
    expect(new TextDecoder().decode(bytes.subarray(38, 58))).toBe('application/epub+zip');

    const container = parseXml(await text(zip, 'META-INF/container.xml'));
    expect(container.getElementsByTagName('rootfile')[0].getAttribute('full-path')).toBe('EPUB/package.opf');

    // Structure.
    for (const p of [
      'EPUB/nav.xhtml',
      'EPUB/toc.ncx',
      'EPUB/css/krosspoint.css',
      'EPUB/articles/index.xhtml',
      'EPUB/articles/steam_engine.xhtml',
      'EPUB/articles/boiler.xhtml',
      'EPUB/articles/piston.xhtml',
      'EPUB/articles/external-links.xhtml',
      'EPUB/articles/missing.xhtml',
      'EPUB/articles/attribution.xhtml',
      'EPUB/images/steam_engine_01.png',
    ]) {
      expect(zip.file(p), p).not.toBeNull();
    }

    // OPF metadata and manifest.
    const opf = parseXml(await text(zip, 'EPUB/package.opf'));
    const dc = (n: string) =>
      Array.from(opf.getElementsByTagNameNS('http://purl.org/dc/elements/1.1/', n)).map((e) => e.textContent);
    expect(dc('title')).toEqual(['Steam engine pack']);
    expect(dc('creator')).toEqual(['Wikipedia contributors']);
    expect(dc('source')).toEqual(['https://en.wikipedia.org/wiki/Steam_engine']);
    expect(dc('subject')).toEqual(['K-rosspoint pack']);
    const manifestIds = new Set<string>();
    for (const item of Array.from(opf.getElementsByTagName('item'))) {
      const id = item.getAttribute('id') as string;
      expect(manifestIds.has(id), id).toBe(false);
      manifestIds.add(id);
      expect(zip.file(`EPUB/${item.getAttribute('href')}`), item.getAttribute('href') ?? '').not.toBeNull();
    }

    // Every XHTML file parses, has unique ids and no remote image.
    for (const name of Object.keys(zip.files).filter((n) => n.endsWith('.xhtml'))) {
      const src = await text(zip, name);
      const doc = parseXml(src, XHTML);
      const ids = Array.from(doc.querySelectorAll('[id]')).map((e) => e.getAttribute('id'));
      expect(new Set(ids).size, name).toBe(ids.length);
      expect(src).not.toMatch(/src="http/);
    }

    // Nav: nested headings per article.
    const nav = parseXml(await text(zip, 'EPUB/nav.xhtml'), XHTML);
    const top = Array.from(nav.querySelector('nav > ol')!.children).map((li) => li.querySelector('a')!.textContent);
    expect(top).toEqual([
      'Title page',
      'Steam engine',
      'Boiler',
      'Piston',
      'External links',
      'Missing linked articles',
      'Sources and attribution',
    ]);
    const steam = nav.querySelector('nav > ol')!.children[1];
    expect(steam.querySelector(':scope > ol a')!.getAttribute('href')).toBe('articles/steam_engine.xhtml#Design');
    expect(steam.querySelector(':scope > ol ol a')!.textContent).toBe('Valves');

    // NCX: unique ids, gap-free playOrder.
    const ncx = parseXml(await text(zip, 'EPUB/toc.ncx'));
    const points = Array.from(ncx.getElementsByTagName('navPoint'));
    const npIds = points.map((p) => p.getAttribute('id'));
    expect(new Set(npIds).size).toBe(points.length);
    const orders = points.map((p) => Number(p.getAttribute('playOrder'))).sort((a, b) => a - b);
    expect(orders).toEqual(orders.map((_, i) => i + 1));

    // Article content.
    const steamX = await text(zip, 'EPUB/articles/steam_engine.xhtml');
    expect(steamX).toContain('<h1>Steam engine</h1>');
    expect(steamX).toContain('class="source">Source: Wikipedia, captured 2026-10-10</p>');
    expect(steamX).toContain('href="boiler.xhtml"');
    expect(steamX).toContain('href="piston.xhtml#History"');
    expect(steamX).toContain('href="https://en.wikipedia.org/wiki/James_Watt"');
    expect(steamX).toContain('href="https://example.org/ref"');
    expect(steamX).toContain('src="../images/steam_engine_01.png"');
    expect(steamX).toContain('href="../css/krosspoint.css"');

    // Attribution lists the three URLs and the license.
    const attr = await text(zip, 'EPUB/articles/attribution.xhtml');
    for (const t of ['Steam_engine', 'Boiler', 'Piston']) {
      expect(attr).toContain(`href="https://en.wikipedia.org/wiki/${t}"`);
    }
    expect(attr).toContain('Creative Commons Attribution-ShareAlike 4.0 License');
    expect(attr).toContain('Generated by k-rosspointSender');

    // Missing: the not-bundled link target and the failed page.
    const missing = await text(zip, 'EPUB/articles/missing.xhtml');
    expect(missing).toContain('https://en.wikipedia.org/wiki/James_Watt');
    expect(missing).toContain('Condenser');

    // External links grouped by the holding article.
    const ext = await text(zip, 'EPUB/articles/external-links.xhtml');
    expect(ext).toContain('<h2><a href="steam_engine.xhtml">Steam engine</a></h2>');
    expect(ext).toContain('James Watt');

    // Index lists the articles.
    const index = await text(zip, 'EPUB/articles/index.xhtml');
    expect(index).toContain('href="boiler.xhtml"');

    // Progress.
    expect(events.filter((e) => e.phase === 'building').length).toBe(3);
    expect(events[events.length - 1].phase).toBe('complete');
    expect(events[events.length - 1].errors.map((e) => e.key)).toEqual(['Condenser']);
  });

  it('keeps at most maxImagesPerArticle images per page', async () => {
    const pack = samplePack({ maxImagesPerArticle: 1 });
    const p = pack.pages[0];
    p.article = { ...(p.article as Article), bodyHtml: `<p>x</p><img src="${IMG}" alt="a"><img src="${IMG}2" alt="b">` };
    const images = new Map([
      [IMG, new Blob([PNG_1X1], { type: 'image/png' })],
      [`${IMG}2`, new Blob([PNG_1X1], { type: 'image/png' })],
    ]);
    const f = await buildPackEpub(pack, buildOpts(), images);
    const zip = await JSZip.loadAsync(await blobBytes(f.blob));
    expect(zip.file('EPUB/images/steam_engine_01.png')).not.toBeNull();
    expect(zip.file('EPUB/images/steam_engine_02.png')).toBeNull();
    expect(await text(zip, 'EPUB/articles/steam_engine.xhtml')).toContain('class="img-alt">b</p>');
  });

  it('drops images from the largest pages to meet the size limit', async () => {
    // A PNG header and 300 KB of noise. Without a canvas the image passes through.
    const noise = new Uint8Array(300 * 1024);
    let x = 2463534242;
    for (let i = 0; i < noise.length; i++) {
      x ^= x << 13;
      x ^= x >>> 17;
      x ^= x << 5;
      noise[i] = x & 0xff;
    }
    const big = new Uint8Array(PNG_1X1.length + noise.length);
    big.set(PNG_1X1);
    big.set(noise, PNG_1X1.length);
    const images = new Map([[IMG, new Blob([big], { type: 'image/png' })]]);

    const events: PackProgress[] = [];
    const f = await buildPackEpub(samplePack({ maxTotalEpubSizeMB: 0.1 }), buildOpts(), images, (p) => events.push(p));
    expect(f.blob.size).toBeLessThan(0.1 * 1024 * 1024);
    const zip = await JSZip.loadAsync(await blobBytes(f.blob));
    expect(zip.file('EPUB/images/steam_engine_01.png')).toBeNull();
    const opf = await text(zip, 'EPUB/package.opf');
    expect(opf).not.toContain('images/');
    expect(await text(zip, 'EPUB/articles/steam_engine.xhtml')).toContain('class="img-alt">Engine</p>');
    expect(events[events.length - 1].message).toMatch(/Removed the images/);
  });

  it('throws a clear error when the pack is still too large', async () => {
    const images = new Map([[IMG, new Blob([PNG_1X1], { type: 'image/png' })]]);
    await expect(buildPackEpub(samplePack({ maxTotalEpubSizeMB: 0.001 }), buildOpts(), images)).rejects.toThrow(
      /^Pack is [\d.]+ MB; the limit is 0\.001 MB\. Lower the article count or the image limit\.$/,
    );
  });

  it('uses "Various" as creator for web packs', async () => {
    const pack = { ...samplePack(), kind: 'web' as const };
    const f = await buildPackEpub(pack, buildOpts());
    const zip = await JSZip.loadAsync(await blobBytes(f.blob));
    const opf = await text(zip, 'EPUB/package.opf');
    expect(opf).toContain('<dc:creator id="creator">Various</dc:creator>');
    expect(await text(zip, 'EPUB/articles/attribution.xhtml')).not.toContain('ShareAlike');
  });
});
