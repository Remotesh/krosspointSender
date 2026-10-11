/**
 * Library drawer, packs section. Per pack: open, re-export, send, delete.
 * Re-export builds the EPUB again from the stored pages and the cached
 * images (when the library has them). It does not fetch again.
 */
import type { OutputFile } from '../types';
import { deletePack, listPackEntries, type StoredPack } from '../storage';
import { errorMessage, formatBytes, formatDate, h, replace } from './dom';
import { blobsToMap, downloadFile, sendFile } from './actions';
import { buildPackFile, savePackToLibrary } from './packOutput';
import { store } from './state';
import { toast } from './toast';

export interface PacksSection {
  el: HTMLElement;
  reload(): Promise<void>;
}

/** Build a stored pack again. The filename of the last export stays. */
async function rebuild(e: StoredPack): Promise<{ file: OutputFile; images: Map<string, Blob> }> {
  const images = blobsToMap(e.imageBlobs) ?? new Map<string, Blob>();
  const file = await buildPackFile(e.pack, images, '');
  if (e.fileName) file.name = e.fileName;
  return { file, images };
}

export function createPacksSection(onOpen: () => void): PacksSection {
  const st = store();
  const list = h('ul', { class: 'lib-list' });
  const count = h('span', { class: 'hint' });
  let entries: StoredPack[] = [];

  const act = (label: string, fn: () => Promise<void>, cls = 'btn small'): HTMLButtonElement =>
    h('button', {
      type: 'button',
      class: cls,
      onclick: async (ev: Event) => {
        const b = ev.currentTarget as HTMLButtonElement;
        b.disabled = true;
        try {
          await fn();
        } catch (err) {
          toast(`${label} failed: ${errorMessage(err)}`, 'error');
        } finally {
          b.disabled = false;
        }
      },
    }, label);

  function row(e: StoredPack): HTMLElement {
    const p = e.pack;
    const done = p.pages.filter((x) => x.state === 'done' && x.article);
    const words = done.reduce((n, x) => n + (x.article?.wordCount ?? 0), 0);
    const cached = e.imageBlobs ? Object.keys(e.imageBlobs).length : 0;
    const last = e.sends?.[e.sends.length - 1];
    return h(
      'li',
      { class: 'lib-item lib-pack' },
      h(
        'div',
        { class: 'lib-main' },
        h('strong', null, p.title),
        h(
          'small',
          null,
          [p.kind === 'wikipedia' ? 'Wikipedia pack' : 'Web pack', `${done.length} articles`, `${words.toLocaleString()} words`, cached ? `${cached} images cached` : '']
            .filter(Boolean)
            .join(' · '),
        ),
        h('small', null, `Saved ${formatDate(e.updatedAt)}${e.fileName ? ` · ${e.fileName}` : ''}`),
        last
          ? h('small', { class: last.ok ? 'ok' : 'bad' }, `Last send: ${last.ok ? 'OK' : 'failed'} via ${last.method}${last.deviceName ? ` to ${last.deviceName}` : ''}, ${formatDate(last.at)}`)
          : null,
        h(
          'div',
          { class: 'btn-row' },
          act('Open', async () => {
            const { file, images } = await rebuild(e);
            st.set({
              pack: p,
              packOutput: { file, images, builtTitle: p.title, builtAuthor: '' },
              packAuthor: '',
              packFileName: e.fileName,
              packPreviewKey: undefined,
              mode: 'pack',
              transfer: { state: 'idle' },
            });
            window.dispatchEvent(new CustomEvent('krosspoint:pack-opened'));
            onOpen();
          }),
          act('Re-export', async () => {
            const { file } = await rebuild(e);
            downloadFile(file);
            await savePackToLibrary(p, file.name);
            toast(`Exported ${file.name} (${formatBytes(file.blob.size)})`, 'ok');
          }),
          act('Send', async () => {
            const dev = st.selectedProfile();
            if (!dev) {
              toast('Choose a device in the Device panel first.', 'warn');
              return;
            }
            const { file } = await rebuild(e);
            try {
              await sendFile(dev, file);
              await savePackToLibrary(p, file.name, undefined, { ok: true, method: dev.method, deviceName: dev.name });
            } catch (err) {
              await savePackToLibrary(p, file.name, undefined, { ok: false, method: dev.method, deviceName: dev.name, error: errorMessage(err) });
              throw err;
            }
            toast(`Sent ${file.name} to ${dev.name}.`, 'ok');
          }),
          act('Delete', async () => {
            if (!confirm(`Delete the pack "${p.title}" from the library?`)) return;
            await deletePack(p.id);
            await reload();
          }, 'btn small danger'),
        ),
      ),
    );
  }

  async function reload(): Promise<void> {
    try {
      entries = await listPackEntries();
    } catch (err) {
      toast(`Could not read the packs: ${errorMessage(err)}`, 'error');
      entries = [];
    }
    count.textContent = `${entries.length} pack(s)`;
    replace(list, ...(entries.length ? entries.map(row) : [h('li', { class: 'hint' }, 'No packs yet. Build one in the [ pack ] tab.')]));
  }

  return { el: h('section', { class: 'lib-packs' }, h('h3', null, 'Packs'), count, list), reload };
}
