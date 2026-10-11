import { describe, expect, it } from 'vitest';
import { detectInputKind, isWikipediaUrl, normalizeUrl } from '../../src/extractors';

describe('normalizeUrl', () => {
  it('trims and adds https://', () => {
    expect(normalizeUrl('  example.com/a  ')).toBe('https://example.com/a');
    expect(normalizeUrl('www.example.com')).toBe('https://www.example.com/');
    expect(normalizeUrl('//cdn.example.com/x')).toBe('https://cdn.example.com/x');
  });

  it('keeps http and lowercases the host', () => {
    expect(normalizeUrl('http://EXAMPLE.com/Path')).toBe('http://example.com/Path');
  });

  it('rejects non-http(s) and junk', () => {
    expect(normalizeUrl('javascript:alert(1)')).toBeNull();
    expect(normalizeUrl('ftp://example.com/file')).toBeNull();
    expect(normalizeUrl('mailto:a@b.c')).toBeNull();
    expect(normalizeUrl('data:text/html,hi')).toBeNull();
    expect(normalizeUrl('')).toBeNull();
    expect(normalizeUrl('hello world')).toBeNull();
    expect(normalizeUrl('notaurl')).toBeNull();
  });

  it('strips tracking parameters and keeps the rest', () => {
    expect(normalizeUrl('https://ex.com/a?utm_source=x&utm_medium=y&id=5&fbclid=abc')).toBe('https://ex.com/a?id=5');
    expect(normalizeUrl('https://ex.com/a?utm_source=x&gclid=1')).toBe('https://ex.com/a');
    expect(normalizeUrl('https://ex.com/a?utm_campaign=z#part')).toBe('https://ex.com/a#part');
  });

  it('accepts localhost and IPs', () => {
    expect(normalizeUrl('localhost:5173/x')).toBe('https://localhost:5173/x');
    expect(normalizeUrl('http://192.168.1.50/')).toBe('http://192.168.1.50/');
  });
});

describe('isWikipediaUrl', () => {
  it('detects Wikipedia article URLs', () => {
    expect(isWikipediaUrl('https://en.wikipedia.org/wiki/Sourdough')).toBe(true);
    expect(isWikipediaUrl('de.m.wikipedia.org/wiki/Brot')).toBe(true);
    expect(isWikipediaUrl('https://en.wikipedia.org/w/index.php?title=Bread')).toBe(true);
    expect(isWikipediaUrl('https://wikipedia.org.evil.com/wiki/X')).toBe(false);
    expect(isWikipediaUrl('https://example.com/wiki/X')).toBe(false);
    expect(isWikipediaUrl('https://en.wikipedia.org/')).toBe(false);
  });
});

describe('detectInputKind', () => {
  it('detects URLs and Wikipedia URLs', () => {
    expect(detectInputKind('https://example.com/post')).toBe('url');
    expect(detectInputKind('example.com/post')).toBe('url');
    expect(detectInputKind('  https://en.wikipedia.org/wiki/Bread \n')).toBe('wikipedia');
  });

  it('detects HTML', () => {
    expect(detectInputKind('<!DOCTYPE html><html><body><p>x</p></body></html>')).toBe('html');
    expect(detectInputKind('<p>Hello</p><p>World</p>')).toBe('html');
    expect(detectInputKind('<article>Only one tag</article>')).toBe('html');
  });

  it('detects Markdown', () => {
    expect(detectInputKind('# Title\n\nSome text with a [link](https://x.y).')).toBe('markdown');
    expect(detectInputKind('Intro\n\n- one\n- two\n\n**bold** text')).toBe('markdown');
    expect(detectInputKind('```\ncode\n```')).toBe('markdown');
  });

  it('detects plain text', () => {
    expect(detectInputKind('Just a sentence. And another one.\n\nA second paragraph.')).toBe('text');
    expect(detectInputKind('')).toBe('text');
    expect(detectInputKind('a < b and c > d')).toBe('text');
  });

  it('detects a bookmarklet payload', () => {
    expect(detectInputKind('{"v":1,"url":"https://x.y","html":"<p>a</p>","capturedAt":"2026-01-01"}')).toBe('bookmarklet');
  });

  it('lets the file extension win', () => {
    expect(detectInputKind('# not markdown?', 'notes.txt')).toBe('text');
    expect(detectInputKind('plain words', 'README.md')).toBe('markdown');
    expect(detectInputKind('plain words', 'page.HTM')).toBe('html');
    expect(detectInputKind('<p>a</p><p>b</p>', 'unknown.bin')).toBe('html');
  });
});
