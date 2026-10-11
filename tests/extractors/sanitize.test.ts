import { describe, expect, it } from 'vitest';
import { ALLOWED_ATTR, ALLOWED_TAGS, buildToc, countWords, markdownToHtml, sanitizeHtml, textToHtml } from '../../src/extractors';

function tagsIn(html: string): Set<string> {
  const doc = new DOMParser().parseFromString(`<body>${html}</body>`, 'text/html');
  return new Set(Array.from(doc.body.querySelectorAll('*')).map((e) => e.tagName.toLowerCase()));
}

function attrsIn(html: string): Set<string> {
  const doc = new DOMParser().parseFromString(`<body>${html}</body>`, 'text/html');
  const out = new Set<string>();
  for (const el of Array.from(doc.body.querySelectorAll('*'))) for (const a of Array.from(el.attributes)) out.add(a.name);
  return out;
}

describe('sanitizeHtml allowlist', () => {
  const dirty = `
    <div class="wrap" style="color:red" onclick="evil()">
      <script>alert(1)</script><style>p{}</style>
      <nav><a href="/home">Home</a></nav>
      <header><p>Site header</p></header>
      <h2 class="t" data-x="1">Heading</h2>
      <p id="keep" title="drop" onmouseover="x()">Text <font color="red">old</font> <span style="x">span</span>
        <a href="/rel/page" target="_blank" rel="noopener">rel</a>
        <a href="javascript:alert(1)">js</a>
        <a href="data:text/html,<b>x</b>">data</a>
        <a href="#frag">frag</a>
      </p>
      <img src="pic.png" alt="A pic" width="10" height="10" onerror="x()">
      <img src="data:image/png;base64,iVBORw0KGgo=" alt="inline">
      <iframe src="https://evil.example"></iframe>
      <form><input value="x"></form>
      <table border="1"><tr><td colspan="2" rowspan="1" class="c">cell</td></tr><tr><td>a</td><td>b</td></tr></table>
      <video src="x.mp4"></video>
      <aside>Aside text</aside>
      <footer>Footer text</footer>
      <div role="navigation">Role nav</div>
    </div>`;

  const out = sanitizeHtml(dirty, { baseUrl: 'https://site.example/blog/post' });

  it('keeps only allowlisted tags', () => {
    for (const t of tagsIn(out)) expect(ALLOWED_TAGS).toContain(t);
  });

  it('keeps only allowlisted attributes', () => {
    for (const a of attrsIn(out)) expect(ALLOWED_ATTR).toContain(a);
    expect(ALLOWED_ATTR).toEqual(['href', 'src', 'alt', 'id', 'colspan', 'rowspan']);
  });

  it('removes scripts, chrome and their content', () => {
    expect(out).not.toMatch(/alert|<script|<style|<iframe|<form|<video/);
    expect(out).not.toContain('Site header');
    expect(out).not.toContain('Aside text');
    expect(out).not.toContain('Footer text');
    expect(out).not.toContain('Role nav');
    expect(out).not.toContain('Home');
  });

  it('resolves relative URLs and drops bad hrefs', () => {
    expect(out).toContain('href="https://site.example/rel/page"');
    expect(out).toContain('src="https://site.example/blog/pic.png"');
    expect(out).toContain('href="#frag"');
    expect(out).not.toMatch(/href="(javascript|data):/);
    expect(out).toContain('js'); // The link text stays.
  });

  it('keeps data: image sources', () => {
    expect(out).toContain('src="data:image/png;base64,iVBORw0KGgo="');
  });

  it('keeps colspan, rowspan and id', () => {
    expect(out).toContain('colspan="2"');
    expect(out).toContain('id="keep"');
  });

  it('can drop links and images', () => {
    const o = sanitizeHtml('<p>A <a href="https://x.y">link</a> <img src="https://x.y/a.jpg" alt="i"></p>', { keepLinks: false, keepImages: false });
    expect(o).not.toContain('<a');
    expect(o).not.toContain('<img');
    expect(o).toContain('link');
  });

  it('linearizes divs and sections into paragraphs', () => {
    const o = sanitizeHtml('<div><div>One</div><section><div>Two</div><p>Three</p></section>Loose <b>four</b></div>');
    expect(o).toBe('<p>One</p><p>Two</p><p>Three</p><p>Loose <b>four</b></p>');
  });

  it('turns figure layouts into figure, img, figcaption', () => {
    const o = sanitizeHtml(
      '<figure class="x"><div class="inner"><picture><source srcset="/a.webp" type="image/webp"><img src="/a.jpg" alt="A"></picture></div><figcaption><p>Cap</p></figcaption></figure>',
      { baseUrl: 'https://s.example/' },
    );
    expect(o).toBe('<figure><img src="https://s.example/a.jpg" alt="A"><figcaption>Cap</figcaption></figure>');
  });

  it('linearizes tables when keepTables is false', () => {
    const o = sanitizeHtml('<table><tr><th>A</th><th>B</th></tr><tr><td>1</td><td>2</td></tr></table>', { keepTables: false });
    expect(o).toBe('<p>A | B</p><p>1 | 2</p>');
  });

  it('unwraps layout tables', () => {
    const o = sanitizeHtml('<table><tr><td><p>Layout</p><table><tr><td>a</td><td>b</td></tr></table></td></tr></table>');
    expect(o).toContain('<p>Layout</p>');
    expect((o.match(/<table>/g) ?? []).length).toBe(1);
  });

  it('keeps pre and code', () => {
    const o = sanitizeHtml('<pre><code class="lang-js">let a = 1;\n  a++;</code></pre>');
    expect(o).toBe('<pre><code>let a = 1;\n  a++;</code></pre>');
  });
});

describe('buildToc', () => {
  it('gives every heading a sec-N id and lists them', () => {
    const { bodyHtml, toc } = buildToc('<h1>Top</h1><p>x</p><h2 id="old">Sub <em>part</em></h2><h3></h3><p><a href="#old">jump</a></p><p id="sec-9">p</p>');
    expect(toc).toEqual([
      { id: 'sec-1', level: 1, text: 'Top' },
      { id: 'sec-2', level: 2, text: 'Sub part' },
    ]);
    expect(bodyHtml).toContain('<h1 id="sec-1">');
    expect(bodyHtml).toContain('<h2 id="sec-2">');
    expect(bodyHtml).toContain('<h3 id="sec-3">');
    expect(bodyHtml).toContain('href="#sec-2"');
    expect(bodyHtml).not.toContain('id="sec-9"');
  });
});

describe('countWords', () => {
  it('counts whitespace-separated tokens across blocks', () => {
    expect(countWords('<p>one two</p><p>three</p><ul><li>four</li><li>five six</li></ul>')).toBe(6);
    expect(countWords('')).toBe(0);
  });
});

describe('markdownToHtml and textToHtml', () => {
  it('converts Markdown and strips scripts', () => {
    const html = markdownToHtml('# T\n\nHello **world**\n\n<script>alert(1)</script>');
    expect(html).toContain('<h1>T</h1>');
    expect(html).toContain('<strong>world</strong>');
    expect(html).not.toContain('script');
  });

  it('makes paragraphs on blank lines and escapes', () => {
    expect(textToHtml('a <b> & c\n\nsecond')).toBe('<p>a &lt;b&gt; &amp; c</p>\n<p>second</p>');
    expect(textToHtml('short\nlines')).toBe('<p>short<br>lines</p>');
  });
});
