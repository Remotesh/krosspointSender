/**
 * Library drawer. It lists saved articles. Per article: open, re-export,
 * send, delete. For many: bundle into one EPUB. Also clear all and
 * export the library as JSON.
 */
import type { LibraryEntry } from '../types';
import { buildBundleEpub } from '../epub';
import { clearLibrary, deleteArticle, exportLibraryJson, listArticles } from '../storage';
import { createDrawer, errorMessage, formatBytes, formatDate, h, replace, type Drawer } from './dom';
import {
  blobsToMap,
  buildOptions,
  buildOutput,
  downloadFile,
  imagesFor,
  libraryChanged,
  saveToLibrary,
  sendFile,
} from './actions';
import { store } from './state';
import { createPacksSection } from './libraryPacks';
import { toast } from './toast';

export function createLibraryDrawer(): Drawer {
  const st = store();
  const selected = new Set<string>();
  let entries: LibraryEntry[] = [];
  const list = h('ul', { class: 'lib-list' });
  const count = h('span', { class: 'hint' });
  const btnBundle = h('button', { type: 'button', class: 'btn primary' }, 'Bundle into one EPUB');
  const btnAll = h('button', { type: 'button', class: 'btn' }, 'Select all');
  const btnJson = h('button', { type: 'button', class: 'btn' }, 'Export JSON');
  const btnClear = h('button', { type: 'button', class: 'btn danger' }, 'Clear all');

  const drawer = createDrawer('Library', () => void reload());
  const packs = createPacksSection(() => drawer.close());
  replace(
    drawer.body,
    h('p', { class: 'hint' }, 'Articles and packs stay in this browser (IndexedDB). Nothing leaves your machine.'),
    h('div', { class: 'btn-row' }, btnBundle, btnAll, btnJson, btnClear),
    packs.el,
    h('h3', null, 'Articles'),
    count,
    list,
  );

  async function reload(): Promise<void> {
    try {
      entries = await listArticles();
      void packs.reload();
    } catch (err) {
      toast(`Could not read the library: ${errorMessage(err)}`, 'error');
      entries = [];
    }
    for (const id of [...selected]) if (!entries.some((e) => e.article.id === id)) selected.delete(id);
    render();
  }

  function render(): void {
    count.textContent = `${entries.length} article(s), ${selected.size} selected`;
    btnBundle.disabled = selected.size < 2;
    if (!entries.length) {
      replace(list, h('li', { class: 'hint' }, 'The library is empty. Export or send an article to save it here.'));
      return;
    }
    replace(list, ...entries.map(row));
  }

  function row(e: LibraryEntry): HTMLElement {
    const a = e.article;
    const box = h('input', { type: 'checkbox', checked: selected.has(a.id), 'aria-label': `Select ${a.title}` });
    box.addEventListener('change', () => {
      if (box.checked) selected.add(a.id);
      else selected.delete(a.id);
      render();
    });
    const last = e.transfers[e.transfers.length - 1];
    const cached = e.imageBlobs ? Object.keys(e.imageBlobs).length : 0;
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
    return h(
      'li',
      { class: 'lib-item' },
      h('label', { class: 'lib-check' }, box),
      h(
        'div',
        { class: 'lib-main' },
        h('strong', null, a.title),
        h(
          'small',
          null,
          [a.siteName, a.author, `${a.wordCount} words`, `.${e.lastFormat}`, cached ? `${cached} images cached` : '']
            .filter(Boolean)
            .join(' · '),
        ),
        h('small', null, `Saved ${formatDate(e.updatedAt)}`),
        last
          ? h(
              'small',
              { class: last.ok ? 'ok' : 'bad' },
              `Last send: ${last.ok ? 'OK' : 'failed'} via ${last.method}${last.deviceName ? ` to ${last.deviceName}` : ''}, ${formatDate(last.at)}`,
            )
          : null,
        h(
          'div',
          { class: 'btn-row' },
          act('Open', async () => {
            st.set({ article: e.article, transfer: { state: 'idle' } });
            if (e.imageBlobs) await imagesFor(e.article, blobsToMap(e.imageBlobs));
            drawer.close();
          }),
          act('Re-export', async () => {
            const { file, images } = await buildOutput(a, st.get().format, st.get().settings, blobsToMap(e.imageBlobs));
            downloadFile(file);
            await saveToLibrary(a, file, images);
            toast(`Exported ${file.name} (${formatBytes(file.blob.size)})`, 'ok');
          }),
          act('Send', async () => {
            const p = st.selectedProfile();
            if (!p) {
              toast('Choose a device in the Device panel first.', 'warn');
              return;
            }
            const { file, images } = await buildOutput(a, st.get().format, st.get().settings, blobsToMap(e.imageBlobs));
            await saveToLibrary(a, file, images);
            await sendFile(p, file, a.id);
            toast(`Sent ${file.name} to ${p.name}.`, 'ok');
          }),
          act('Delete', async () => {
            if (!confirm(`Delete "${a.title}" from the library?`)) return;
            await deleteArticle(a.id);
            selected.delete(a.id);
            await reload();
          }, 'btn small danger'),
        ),
      ),
    );
  }

  btnAll.addEventListener('click', () => {
    if (selected.size === entries.length) selected.clear();
    else for (const e of entries) selected.add(e.article.id);
    render();
  });

  btnBundle.addEventListener('click', async () => {
    const chosen = entries.filter((e) => selected.has(e.article.id)).reverse(); // oldest first
    if (chosen.length < 2) return;
    const title = prompt('Title for the bundle EPUB:', `Reading bundle ${new Date().toISOString().slice(0, 10)}`);
    if (!title) return;
    btnBundle.disabled = true;
    try {
      const settings = st.get().settings;
      const images = new Map<string, Blob>();
      if (settings.build.includeImages) {
        for (const e of chosen) {
          const m = await imagesFor(e.article, blobsToMap(e.imageBlobs));
          for (const [k, v] of m) images.set(k, v);
        }
      }
      const file = await buildBundleEpub(
        chosen.map((e) => e.article),
        { title },
        buildOptions(settings),
        images,
      );
      downloadFile(file);
      toast(`Bundle saved: ${file.name} (${formatBytes(file.blob.size)})`, 'ok');
      const p = st.selectedProfile();
      if (p && p.method !== 'download' && confirm(`Also send the bundle to ${p.name}?`)) {
        await sendFile(p, file);
      }
    } catch (err) {
      toast(`Bundle failed: ${errorMessage(err)}`, 'error');
    } finally {
      render();
    }
  });

  btnJson.addEventListener('click', async () => {
    try {
      const blob = await exportLibraryJson();
      downloadFile({ name: 'krosspoint-library.json', blob });
    } catch (err) {
      toast(`Export failed: ${errorMessage(err)}`, 'error');
    }
  });

  btnClear.addEventListener('click', async () => {
    if (!confirm('Delete every article and pack in the library? This cannot be undone.')) return;
    try {
      await clearLibrary();
      selected.clear();
      libraryChanged();
    } catch (err) {
      toast(`Clear failed: ${errorMessage(err)}`, 'error');
    }
  });

  window.addEventListener('krosspoint:library-changed', () => {
    if (drawer.isOpen()) void reload();
  });

  return drawer;
}
