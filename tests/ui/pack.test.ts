import { beforeEach, describe, expect, it } from 'vitest';
import JSZip from 'jszip';
import type { OutputFile, Pack, PackCandidate } from '../../src/types';
import { mergeSettings } from '../../src/settings';
import { createStore, hasOutput, setStore, viewArticle, type Store } from '../../src/ui/state';
import { manualCandidate, renderSelectStep, selectedCount } from '../../src/ui/packSelect';
import { patchCreator } from '../../src/ui/packOutput';
import { parentFolder } from '../../src/ui/folderBrowser';

function cand(key: string, depth: number, selected: boolean): PackCandidate {
  return { key, title: key, url: `https://en.wikipedia.org/wiki/${key.replace(/ /g, '_')}`, depth, linkCount: 1, selected };
}

function pack(max = 3): Pack {
  return {
    id: 'pk',
    kind: 'wikipedia',
    title: 'Steam engine pack',
    rootKey: 'Steam engine',
    rootUrl: 'https://en.wikipedia.org/wiki/Steam_engine',
    limits: {
      depth: 1,
      maxArticles: max,
      maxImagesPerArticle: 2,
      maxTotalEpubSizeMB: 25,
      requestDelayMs: 0,
      includeReferences: false,
      includeInfobox: false,
      sameDomainOnly: true,
    },
    pages: [cand('Steam engine', 0, true), cand('Boiler', 1, true), cand('Piston', 1, false), cand('Watt', 1, false), cand('Cylinder', 1, false)].map(
      (c) => ({ candidate: c, state: 'queued' as const }),
    ),
    externalLinks: [],
    createdAt: '',
    updatedAt: '',
  };
}

let st: Store;
beforeEach(() => {
  st = createStore({
    settings: mergeSettings(undefined),
    profiles: [],
    format: 'epub',
    transfer: { state: 'idle' },
    showImages: true,
    zoom: 1,
    busy: false,
    mode: 'pack',
    pack: pack(),
  });
  setStore(st);
});

function mount(): HTMLElement {
  const el = renderSelectStep(() => st.get().pack!, { update: (p) => st.set({ pack: p }) });
  document.body.replaceChildren(el);
  return el;
}

describe('pack select step', () => {
  it('shows the counter and blocks a selection over the limit', () => {
    const el = mount();
    expect(el.querySelector('.pack-counter')!.textContent).toContain('2 selected / max 3');
    const boxes = () => Array.from(el.querySelectorAll<HTMLInputElement>('.pack-cand input[type=checkbox]'));
    boxes()[2].click(); // Piston: 3 of 3
    expect(selectedCount(st.get().pack!)).toBe(3);
    boxes()[3].click(); // Watt: over the limit
    expect(selectedCount(st.get().pack!)).toBe(3);
    expect(boxes()[3].checked).toBe(false);
    expect(el.querySelector('.pack-counter')!.classList.contains('full')).toBe(true);
  });

  it('filters, selects none (root stays), removes a row and adds a page', () => {
    const el = mount();
    const btn = (t: string) => Array.from(el.querySelectorAll('button')).find((b) => b.textContent === t)!;
    btn('Select none').click();
    expect(st.get().pack!.pages.filter((p) => p.candidate.selected).map((p) => p.candidate.key)).toEqual(['Steam engine']);
    const search = el.querySelector<HTMLInputElement>('input[type=search]')!;
    search.value = 'pis';
    search.dispatchEvent(new Event('input'));
    expect(el.querySelectorAll('.pack-cand').length).toBe(1);
    el.querySelector<HTMLButtonElement>('.pack-cand button')!.click(); // remove Piston
    expect(st.get().pack!.pages.some((p) => p.candidate.key === 'Piston')).toBe(false);
    search.value = '';
    search.dispatchEvent(new Event('input'));
    const add = el.querySelector<HTMLInputElement>('input[type=text]')!;
    add.value = 'https://en.wikipedia.org/wiki/James_Watt';
    btn('Add page').click();
    const added = st.get().pack!.pages.find((p) => p.candidate.key === 'James Watt');
    expect(added?.candidate.selected).toBe(true);
    expect(added?.candidate.flags).toContain('manual');
  });

  it('select all stops at the limit', () => {
    const el = mount();
    Array.from(el.querySelectorAll('button')).find((b) => b.textContent === 'Select all')!.click();
    expect(selectedCount(st.get().pack!)).toBe(3);
  });

  it('manualCandidate rejects empty input and takes web URLs for web packs', () => {
    expect(typeof manualCandidate(pack(), '  ')).toBe('string');
    const web = { ...pack(), kind: 'web' as const, rootUrl: 'https://example.com/a' };
    const c = manualCandidate(web, 'https://example.com/b#x');
    expect(typeof c).toBe('object');
    expect((c as PackCandidate).url).toBe('https://example.com/b');
  });
});

describe('pack state helpers', () => {
  it('viewArticle and hasOutput follow the mode', () => {
    const s = st.get();
    expect(viewArticle(s)).toBeUndefined();
    expect(hasOutput(s)).toBe(false);
    const art = { id: 'a', title: 'Boiler', capturedAt: '', tags: [], bodyHtml: '<p>x</p>', toc: [], images: [], wordCount: 1, inputKind: 'wikipedia' as const };
    const p = pack();
    p.pages[1] = { ...p.pages[1], state: 'done', article: art };
    st.set({ pack: p, packPreviewKey: 'Boiler' });
    expect(viewArticle(st.get())?.title).toBe('Boiler');
    st.set({ mode: 'article' });
    expect(viewArticle(st.get())).toBeUndefined();
  });

  it('parentFolder goes up one level', () => {
    expect(parentFolder('/Books/K-rosspoint/')).toBe('/Books/');
    expect(parentFolder('/Books')).toBe('/');
    expect(parentFolder('/')).toBe('/');
  });
});

describe('patchCreator', () => {
  it('replaces dc:creator and keeps mimetype first and stored', async () => {
    const zip = new JSZip();
    zip.file('mimetype', 'application/epub+zip', { compression: 'STORE' });
    zip.file('EPUB/package.opf', '<package><metadata><dc:title>T</dc:title><dc:creator id="creator">Wikipedia contributors</dc:creator></metadata></package>');
    const blob = await zip.generateAsync({ type: 'blob', compression: 'DEFLATE' });
    const file: OutputFile = { name: 't.epub', mime: 'application/epub+zip', blob, format: 'epub' };
    const out = await patchCreator(file, 'Kobe & Co');
    const back = await JSZip.loadAsync(await out.blob.arrayBuffer());
    expect(Object.keys(back.files)[0]).toBe('mimetype');
    const opf = await back.file('EPUB/package.opf')!.async('string');
    expect(opf).toContain('<dc:creator id="creator">Kobe &amp; Co</dc:creator>');
    // Bytes 30.. hold "mimetype" and the method field (offset 8) is 0 = stored.
    const bytes = new Uint8Array(await out.blob.arrayBuffer());
    expect(new TextDecoder().decode(bytes.slice(30, 38))).toBe('mimetype');
    expect(bytes[8]).toBe(0);
  });
});
