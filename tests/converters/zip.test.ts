import JSZip from 'jszip';
import { describe, expect, it } from 'vitest';
import { buildZip } from '../../src/converters';
import { blobBytes, buildOpts, sampleArticle, sampleImages } from './sample';

describe('buildZip', () => {
  it('bundles epub, html, txt, md and article.json', async () => {
    const a = sampleArticle({ title: 'Zip Test' });
    const f = await buildZip(a, buildOpts(), sampleImages());
    expect(f.format).toBe('zip');
    expect(f.name).toBe('Zip Test.zip');
    expect(f.estimatedPages).toBeGreaterThan(0);
    const zip = await JSZip.loadAsync(await blobBytes(f.blob));
    expect(Object.keys(zip.files).sort()).toEqual(
      ['Zip Test.epub', 'Zip Test.html', 'Zip Test.md', 'Zip Test.txt', 'article.json'].sort(),
    );
    const json = JSON.parse(await zip.file('article.json')!.async('string'));
    expect(json.title).toBe('Zip Test');
    expect(json.bodyHtml).toBe(a.bodyHtml);
    const inner = await JSZip.loadAsync(await zip.file('Zip Test.epub')!.async('uint8array'));
    expect(Object.keys(inner.files)[0]).toBe('mimetype');
  });
});
