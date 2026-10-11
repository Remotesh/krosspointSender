import { describe, expect, it } from 'vitest';
import { DEFAULT_SETTINGS, loadSettings, mergeSettings, resetSettings, saveSettings } from '../../src/settings';
import { getDb } from '../../src/storage';

describe('settings', () => {
  it('has the documented defaults', () => {
    expect(DEFAULT_SETTINGS.style).toMatchObject({
      preset: 'comfortable',
      fontSizePercent: 100,
      marginEm: 0.8,
      lineHeight: 1.45,
      paragraphSpacingEm: 0.6,
      imageMaxWidthPx: 480,
      grayscaleImages: true,
      keepLinks: true,
      hyphenate: false,
      tableHandling: 'keep',
      imageQuality: 0.85,
      customCss: '',
    });
    expect(DEFAULT_SETTINGS.filename).toEqual({ template: '{title}', maxLength: 80, lowercase: false, spaceReplacement: '' });
    expect(DEFAULT_SETTINGS.build).toEqual({ includeImages: true, includeCover: true, includeSourceNote: true });
    expect(DEFAULT_SETTINGS.extract).toEqual({
      keepInfobox: false,
      keepReferences: false,
      keepLinks: true,
      keepImages: true,
      keepTables: true,
    });
    expect(DEFAULT_SETTINGS.cacheImages).toBe(true);
  });

  it('loads defaults when nothing is stored', async () => {
    await resetSettings();
    expect(await loadSettings()).toEqual(mergeSettings(undefined));
  });

  it('merges stored values over the defaults', async () => {
    const db = await getDb();
    // An old, partial record.
    await db.put('settings', { style: { fontSizePercent: 125 }, filename: { lowercase: true }, cacheImages: false } as never, 'app');
    const s = await loadSettings();
    expect(s.style.fontSizePercent).toBe(125);
    expect(s.style.lineHeight).toBe(1.45);
    expect(s.filename.lowercase).toBe(true);
    expect(s.filename.maxLength).toBe(80);
    expect(s.cacheImages).toBe(false);
    expect(s.build.includeCover).toBe(true);
    // A record from before packs existed gets the pack limit defaults.
    expect(s.packLimits).toEqual(DEFAULT_SETTINGS.packLimits);
  });

  it('has pack limit defaults and merges a partial packLimits record', async () => {
    expect(DEFAULT_SETTINGS.packLimits).toEqual({
      depth: 1,
      maxArticles: 20,
      maxImagesPerArticle: 2,
      maxTotalEpubSizeMB: 25,
      requestDelayMs: 500,
      includeReferences: false,
      includeInfobox: false,
      sameDomainOnly: true,
    });
    const m = mergeSettings({ packLimits: { maxArticles: 7, depth: 2 } });
    expect(m.packLimits.maxArticles).toBe(7);
    expect(m.packLimits.depth).toBe(2);
    expect(m.packLimits.requestDelayMs).toBe(500);
    const s = await loadSettings();
    s.packLimits.includeInfobox = true;
    await saveSettings(s);
    expect((await loadSettings()).packLimits.includeInfobox).toBe(true);
    expect(DEFAULT_SETTINGS.packLimits.includeInfobox).toBe(false);
    await resetSettings();
  });

  it('saves, loads and resets', async () => {
    const s = await loadSettings();
    s.style.customCss = 'p{margin:0}';
    s.defaultDeviceId = 'dev-1';
    await saveSettings(s);
    const back = await loadSettings();
    expect(back.style.customCss).toBe('p{margin:0}');
    expect(back.defaultDeviceId).toBe('dev-1');
    const reset = await resetSettings();
    expect(reset.style.customCss).toBe('');
    expect((await loadSettings()).defaultDeviceId).toBeUndefined();
  });

  it('never mutates the defaults', async () => {
    const s = await loadSettings();
    s.style.marginEm = 3;
    expect(DEFAULT_SETTINGS.style.marginEm).toBe(0.8);
  });
});
