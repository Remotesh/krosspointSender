import { describe, expect, it } from 'vitest';
import { estimateReading } from '../../src/converters';
import { PRESETS } from '../../src/epub';
import { sampleArticle } from './sample';

describe('estimateReading', () => {
  it('uses 230 wpm and the preset words per page', () => {
    const a = sampleArticle({ wordCount: 2300 });
    expect(estimateReading(a, PRESETS.comfortable)).toEqual({ minutes: 10, pages: Math.ceil(2300 / 170) });
    // compact: 210 words per page at 100 %, scaled by 90 %.
    expect(estimateReading(a, { ...PRESETS.compact, fontSizePercent: 100 }).pages).toBe(Math.ceil(2300 / 210));
    expect(estimateReading(a, { ...PRESETS.reference, fontSizePercent: 100 }).pages).toBe(Math.ceil(2300 / 190));
  });
  it('scales by font size', () => {
    const a = sampleArticle({ wordCount: 1200 });
    const large = estimateReading(a, { ...PRESETS.large, fontSizePercent: 100 }).pages;
    expect(large).toBe(10);
    expect(estimateReading(a, { ...PRESETS.large, fontSizePercent: 200 }).pages).toBe(20);
  });
  it('counts body words when wordCount is 0', () => {
    const a = sampleArticle({ wordCount: 0, bodyHtml: '<p>one two three</p>' });
    expect(estimateReading(a, PRESETS.comfortable)).toEqual({ minutes: 1, pages: 1 });
  });
});
