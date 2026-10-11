import { afterEach, describe, expect, it, vi } from 'vitest';
import { readPayloadFromHash } from '../../src/extractors';

const buildPath = '../../bookmarklet/build.mjs';

describe('bookmarklet build', () => {
  afterEach(() => vi.unstubAllGlobals());

  it('minifies into a javascript: URL that round-trips through readPayloadFromHash', async () => {
    const mod = await import(/* @vite-ignore */ buildPath);
    const url: string = mod.buildBookmarklet('https://app.example/krosspoint/');
    expect(url.startsWith('javascript:')).toBe(true);
    const code = decodeURIComponent(url.slice('javascript:'.length));
    expect(code).not.toContain('__APP_URL__');
    expect(code).not.toMatch(/\n/);

    document.title = 'Test page — ü';
    document.body.innerHTML = '<article><h1>Hello</h1><p>Grüße from the test.</p></article>';
    const open = vi.fn(() => ({}) as Window);
    vi.stubGlobal('open', open);
    window.open = open as unknown as typeof window.open;

    new Function(code)();

    expect(open).toHaveBeenCalledTimes(1);
    const target = (open.mock.calls[0] as unknown as [string])[0];
    expect(target.startsWith('https://app.example/krosspoint/#payload=')).toBe(true);
    const payload = readPayloadFromHash(new URL(target).hash);
    expect(payload).not.toBeNull();
    expect(payload!.v).toBe(1);
    expect(payload!.title).toBe('Test page — ü');
    expect(payload!.html).toContain('Grüße from the test.');
    expect(payload!.url).toBe(location.href);
  });
});
