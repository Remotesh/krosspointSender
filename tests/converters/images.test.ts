import { afterEach, describe, expect, it, vi } from 'vitest';
import { fetchImages, processImage } from '../../src/converters';
import { GIF_1X1, PNG_1X1, blobBytes, sampleArticle } from './sample';

const opts = { maxWidth: 480, grayscale: true, quality: 0.8 };

describe('processImage without a canvas (jsdom)', () => {
  it('passes a PNG through and reads its size', async () => {
    const r = await processImage(new Blob([PNG_1X1], { type: 'image/png' }), opts);
    expect(r).not.toBeNull();
    expect(r!.mime).toBe('image/png');
    expect(r!.width).toBe(1);
    expect(r!.height).toBe(1);
  });
  it('sniffs the type when the Blob has none', async () => {
    const r = await processImage(new Blob([PNG_1X1]), opts);
    expect(r?.mime).toBe('image/png');
    expect(r?.blob.type).toBe('image/png');
  });
  it('drops a GIF', async () => {
    expect(await processImage(new Blob([GIF_1X1], { type: 'image/gif' }), opts)).toBeNull();
  });
  it('drops bytes that are not an image', async () => {
    expect(await processImage(new Blob(['hello'], { type: 'image/jpeg' }), opts)).toBeNull();
  });
});

describe('fetchImages', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('skips failures and never throws', async () => {
    const fetchMock = vi.fn(async (url: string, init?: RequestInit) => {
      expect(init?.mode).toBe('cors');
      if (url.includes('tea.png')) {
        // Node's Response does not read a jsdom Blob. Pass the bytes.
        return new Response(PNG_1X1, {
          status: 200,
          headers: { 'content-type': 'image/png' },
        });
      }
      if (url.includes('coffee')) return new Response('nope', { status: 404 });
      throw new TypeError('Failed to fetch');
    });
    vi.stubGlobal('fetch', fetchMock);
    const map = await fetchImages(sampleArticle());
    expect(fetchMock).toHaveBeenCalledTimes(3);
    expect([...map.keys()]).toEqual(['https://example.com/tea.png']);
    expect((await blobBytes(map.get('https://example.com/tea.png')!)).length).toBe(PNG_1X1.length);
  });

  it('decodes data: URLs locally', async () => {
    const fetchMock = vi.fn(async () => {
      throw new TypeError('no network');
    });
    vi.stubGlobal('fetch', fetchMock);
    const src = `data:image/png;base64,${btoa(String.fromCharCode(...PNG_1X1))}`;
    const a = sampleArticle({ images: [{ src }], bodyHtml: `<p><img src="${src}" alt="x"></p>` });
    const map = await fetchImages(a);
    expect(fetchMock).not.toHaveBeenCalled();
    expect(map.get(src)?.type).toBe('image/png');
  });
});
