import { describe, expect, it, vi } from 'vitest';

vi.mock('../../src/discovery/findReader', () => ({
  findReader: vi.fn(async (opts: { subnet?: string; profileHosts?: string[] }, onProgress: (p: unknown) => void) => {
    const r = { host: '192.168.1.23', name: 'X4Pro', version: '1.2.0', source: 'sweep' };
    onProgress({ stage: 'sweep', message: 'Sweeping 192.168.1.x (37%)', prefix: '192.168.1', fraction: 0.37, found: [r] });
    return { readers: [r], cancelled: false, notes: [], swept: ['192.168.1'], opts };
  }),
}));

import { findReader } from '../../src/discovery/findReader';
import { createFindReaderBox, readerLabel } from '../../src/ui/findReaderBox';

describe('find reader box', () => {
  it('passes subnet and profile hosts, lists readers, and picks one on click', async () => {
    const onPick = vi.fn();
    const box = createFindReaderBox({ profileHosts: () => ['10.0.0.5'], onPick });
    document.body.append(box.button, box.panel);
    expect(box.panel.hidden).toBe(true);
    box.panel.querySelector<HTMLInputElement>('.subnet-input')!.value = '192.168.1';
    box.button.click();
    await vi.waitFor(() => expect(box.panel.querySelectorAll('.find-results button').length).toBe(1));
    expect(box.panel.hidden).toBe(false);
    const call = vi.mocked(findReader).mock.calls[0][0]!;
    expect(call.subnet).toBe('192.168.1');
    expect(call.profileHosts).toEqual(['10.0.0.5']);
    await vi.waitFor(() => expect(box.button.disabled).toBe(false));
    expect(box.panel.querySelector('.find-progress')!.textContent).toBe('Choose your reader to fill the host.');
    box.panel.querySelector<HTMLButtonElement>('.find-results button')!.click();
    expect(onPick).toHaveBeenCalledWith(expect.objectContaining({ host: '192.168.1.23', name: 'X4Pro' }));
  });

  it('labels a reader', () => {
    expect(readerLabel({ host: 'crosspoint.local', ip: '192.168.0.4', version: '1.2.0', source: 'mdns' })).toBe(
      'crosspoint.local (firmware 1.2.0, 192.168.0.4, via crosspoint.local)',
    );
  });
});
