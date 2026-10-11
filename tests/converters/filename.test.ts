import { describe, expect, it } from 'vitest';
import { makeFilename, sanitizeFilename } from '../../src/converters';
import type { FilenameRules } from '../../src/types';
import { sampleArticle } from './sample';

const rules = (over: Partial<FilenameRules> = {}): FilenameRules => ({
  template: '{title}',
  maxLength: 80,
  lowercase: false,
  spaceReplacement: '',
  ...over,
});

describe('sanitizeFilename', () => {
  it('removes reserved and control characters', () => {
    expect(sanitizeFilename('a<b>c:d"e/f\\g|h?i*j\u0001k')).toBe('a b c d e f g h i j k');
  });
  it('collapses whitespace and trims dots and spaces', () => {
    expect(sanitizeFilename('  ..Hello   \t world.. ')).toBe('Hello world');
  });
  it('never returns an empty name', () => {
    expect(sanitizeFilename('')).toBe('untitled');
    expect(sanitizeFilename('???...')).toBe('untitled');
  });
  it('limits the length', () => {
    const s = sanitizeFilename('x'.repeat(200), { maxLength: 10 });
    expect(s).toBe('x'.repeat(10));
  });
  it('does not split a surrogate pair', () => {
    const s = sanitizeFilename('ab😀😀😀', { maxLength: 3 });
    expect(s).toBe('ab😀');
  });
  it('applies lowercase and space replacement', () => {
    expect(sanitizeFilename('Hello Big - World', { lowercase: true, spaceReplacement: '-' })).toBe('hello-big-world');
    expect(sanitizeFilename('A B', { spaceReplacement: '_' })).toBe('A_B');
  });
  it('guards Windows device names', () => {
    expect(sanitizeFilename('CON')).toBe('_CON');
  });
});

describe('makeFilename', () => {
  it('uses the template and adds the extension', () => {
    const a = sampleArticle({ title: 'My Post' });
    expect(makeFilename(a, 'epub', rules())).toBe('My Post.epub');
    expect(makeFilename(a, 'md', rules({ template: '{date} {title}' }))).toBe('2024-05-01 My Post.md');
    expect(makeFilename(a, 'txt', rules({ template: '{title} - {author}' }))).toBe('My Post - Ada Lovelace.txt');
  });
  it('drops separators of empty tokens', () => {
    const a = sampleArticle({ title: 'My Post', author: undefined });
    expect(makeFilename(a, 'epub', rules({ template: '{title} - {author}' }))).toBe('My Post.epub');
  });
  it('keeps the extension outside the length limit', () => {
    const a = sampleArticle({ title: 'A very long title indeed' });
    expect(makeFilename(a, 'epub', rules({ maxLength: 6 }))).toBe('A very.epub');
  });
  it('sanitizes the title', () => {
    const a = sampleArticle();
    expect(makeFilename(a, 'html', rules())).toBe('Tea & Coffee a history.html');
  });
});
