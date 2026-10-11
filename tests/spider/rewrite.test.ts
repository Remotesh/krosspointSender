import { afterEach, describe, expect, it } from 'vitest';
import { packSlug, rewriteLinks } from '../../src/spider';
import { clearAliases, rememberAlias } from '../../src/spider/linkFilter';
import type { PackPage } from '../../src/types';
import { article, cand, wikiPage } from './helpers';

afterEach(() => clearAliases());

const W = 'https://en.wikipedia.org/wiki/';

function body(html: string): Document {
  return new DOMParser().parseFromString(`<body>${html}</body>`, 'text/html');
}

describe('rewriteLinks (wikipedia)', () => {
  const rootHtml =
    `<p id="intro">A <a href="${W}Heat_engine">heat engine</a> and <a href="${W}Heat_engine#History">its history</a>. ` +
    `<a href="${W}James_Watt">James Watt</a>, <a href="${W}Broken_page">broken</a>, ` +
    `<a href="${W}Steam_engines">redirect</a>, <a href="//en.m.wikipedia.org/wiki/Heat_engine">mobile</a>. ` +
    `<a href="#cite_note-1">[1]</a> <a href="#nowhere">gone</a> <a href="${W}Steam_engine#intro">self</a> ` +
    `<a href="${W}Help:IPA">ipa</a> <a href="https://en.wikipedia.org/w/index.php?title=X&amp;action=edit&amp;redlink=1">red</a> ` +
    `<a href="https://en.wikipedia.org/w/index.php?title=Steam_engine&amp;action=edit&amp;section=1">edit</a> ` +
    `<a href="https://example.com/ref">source</a> <a href="https://de.wikipedia.org/wiki/Dampfmaschine">de</a></p>` +
    `<ol><li id="cite_note-1">Ref</li></ol>`;

  const pages = (): PackPage[] => [
    wikiPage('Steam engine', rootHtml),
    wikiPage('Heat engine', `<p><a href="${W}Steam_engine">back</a> <a href="${W}James_Watt">Watt</a></p>`),
    wikiPage('Broken page'), // Failed: no article.
  ];

  it('rewrites internal links, marks external links, keeps citations and unwraps junk', () => {
    rememberAlias('en', 'Steam engines', 'Steam engine');
    const input = pages();
    const snapshot = JSON.stringify(input);
    const { pages: out, external } = rewriteLinks(input, 'wikipedia');
    expect(JSON.stringify(input)).toBe(snapshot);

    expect(out.map((p) => p.fileName)).toEqual(['steam_engine.xhtml', 'heat_engine.xhtml', 'broken_page.xhtml']);
    const doc = body(out[0].article!.bodyHtml);
    const links = Array.from(doc.querySelectorAll('a')).map((a) => [a.textContent, a.getAttribute('href'), a.getAttribute('class')]);
    expect(links).toEqual([
      ['heat engine', 'heat_engine.xhtml', null],
      ['its history', 'heat_engine.xhtml', null],
      ['James Watt↗', `${W}James_Watt`, 'ext'],
      ['broken↗', `${W}Broken_page`, 'ext'],
      ['redirect', 'steam_engine.xhtml', null],
      ['mobile', 'heat_engine.xhtml', null],
      ['[1]', '#cite_note-1', null],
      ['self', '#intro', null],
      ['source', 'https://example.com/ref', null],
      ['de', 'https://de.wikipedia.org/wiki/Dampfmaschine', null],
    ]);
    // Junk links are unwrapped; their text stays.
    expect(doc.body.textContent).toContain('ipa');
    expect(doc.body.textContent).toContain('gone');
    expect(doc.body.textContent).toContain('edit');
    expect(doc.querySelector('a.ext span.ext')!.textContent).toBe('↗');

    expect(external).toEqual([
      { key: 'James Watt', title: 'James Watt', url: `${W}James_Watt`, fromKey: 'Steam engine' },
      { key: 'Broken page', title: 'Broken page', url: `${W}Broken_page`, fromKey: 'Steam engine' },
    ]);
    expect(body(out[1].article!.bodyHtml).querySelector('a')!.getAttribute('href')).toBe('steam_engine.xhtml');
    expect(out[2].article).toBeUndefined();
  });

  it('is idempotent: a second run adds no second marker', () => {
    const once = rewriteLinks(pages(), 'wikipedia');
    const twice = rewriteLinks(once.pages, 'wikipedia');
    expect(twice.pages[0].article!.bodyHtml).toBe(once.pages[0].article!.bodyHtml);
    expect(twice.pages.map((p) => p.fileName)).toEqual(once.pages.map((p) => p.fileName));
  });

  it('keeps existing file names and avoids clashes and reserved names', () => {
    const input: PackPage[] = [
      wikiPage('Index', '<p>i</p>'),
      wikiPage('Steam engine', '<p>a</p>', { fileName: 'custom.xhtml' }),
      wikiPage('Steam Engine!', '<p>b</p>'),
      wikiPage('Steam—engine', '<p>c</p>'),
    ];
    const { pages: out } = rewriteLinks(input, 'wikipedia');
    expect(out.map((p) => p.fileName)).toEqual(['index_2.xhtml', 'custom.xhtml', 'steam_engine.xhtml', 'steam_engine_2.xhtml']);
  });
});

describe('rewriteLinks (web)', () => {
  it('maps bundled URLs to file names and marks the rest external', () => {
    const p1: PackPage = {
      candidate: cand('https://blog.example.com/post-1', 'https://blog.example.com/post-1', { depth: 0 }),
      state: 'done',
      article: {
        ...article(
          'Post one',
          '<p><a href="https://blog.example.com/post-2/#c">two</a> <a href="https://blog.example.com/post-3">three</a> ' +
            '<a href="https://blog.example.com/post-3">three again</a> <a href="mailto:a@b.c">mail</a></p>',
          'https://blog.example.com/post-1',
        ),
        inputKind: 'url',
      },
    };
    const p2: PackPage = {
      candidate: cand('https://blog.example.com/post-2', 'https://blog.example.com/post-2'),
      state: 'done',
      article: { ...article('Post two', '<p><a href="https://blog.example.com/post-3">3</a></p>', 'https://blog.example.com/post-2'), inputKind: 'url' },
    };
    const { pages: out, external } = rewriteLinks([p1, p2], 'web');
    const a = Array.from(body(out[0].article!.bodyHtml).querySelectorAll('a'));
    expect(a.map((x) => x.getAttribute('href'))).toEqual(['post_two.xhtml', 'https://blog.example.com/post-3', 'https://blog.example.com/post-3', 'mailto:a@b.c']);
    expect(a[1].classList.contains('ext')).toBe(true);
    expect(external).toEqual([{ key: 'https://blog.example.com/post-3', title: 'three', url: 'https://blog.example.com/post-3', fromKey: 'https://blog.example.com/post-1' }]);
  });
});

describe('packSlug', () => {
  it('folds accents, collapses non-alphanumerics and trims', () => {
    expect(packSlug('Steam engine')).toBe('steam_engine');
    expect(packSlug('Café Müller — Ærøskøbing')).toBe('cafe_muller_aeroskobing');
    expect(packSlug('Straße in Łódź')).toBe('strasse_in_lodz');
    expect(packSlug('  __Hello, World!__ ')).toBe('hello_world');
    expect(packSlug('Œuvre à Þingvellir')).toBe('oeuvre_a_thingvellir');
    expect(packSlug('C++ (programming language)')).toBe('c_programming_language');
  });

  it('is never empty and max 48 characters without a trailing "_"', () => {
    expect(packSlug('')).toBe('page');
    expect(packSlug('***')).toBe('page');
    expect(packSlug('東京')).toBe('page');
    const long = packSlug('a'.repeat(47) + ' bcdef');
    expect(long.length).toBeLessThanOrEqual(48);
    expect(long).toBe('a'.repeat(47));
    expect(packSlug('x'.repeat(100))).toHaveLength(48);
  });
});
