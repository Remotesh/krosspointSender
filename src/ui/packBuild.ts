/**
 * Pack wizard steps 4 and 5.
 * Step 4 (build): fetch the selected pages (fetchPackPages), then their
 * images (fetchImages per page, when images are on), then buildPackEpub.
 * It shows a per-page state list, the errors and a cancel button.
 * Step 5 (done): summary. The pack is the output of the right panel.
 */
import type { Pack, PackPage, PackProgress } from '../types';
import { fetchPackPages } from '../spider';
import { errorMessage, formatBytes, h, replace } from './dom';
import { downloadFile } from './actions';
import { buildPackFile, currentPackFile, fetchPackImages, packExtractOptions, packStats, setBuiltPack } from './packOutput';
import { store } from './state';
import { toast } from './toast';

const STATE_TEXT: Record<PackPage['state'], string> = {
  queued: 'queued',
  fetching: 'fetching',
  done: 'done',
  failed: 'failed',
  skipped: 'skipped',
};

function isAbort(err: unknown): boolean {
  return (err as { name?: string } | null)?.name === 'AbortError' || (err as { code?: string } | null)?.code === 'cancelled';
}

export interface BuildStepHooks {
  /** Go to step 5. */
  done(): void;
}

export function renderBuildStep(hooks: BuildStepHooks): HTMLElement {
  const st = store();
  const line = h('div', { class: 'fetch-status', 'aria-live': 'polite' });
  const bar = h('progress', { max: 1, value: 0, 'aria-label': 'Pack progress' });
  const pages = h('ul', { class: 'pack-states' });
  const errors = h('ul', { class: 'bad pack-errors' });
  const btnRun = h('button', { type: 'button', class: 'btn primary' }, 'Build pack');
  const btnCancel = h('button', { type: 'button', class: 'btn', disabled: true }, 'Cancel');
  let ctrl: AbortController | undefined;

  function selectedPages(pack: Pack): PackPage[] {
    return pack.pages.filter((p) => p.candidate.selected || p.state === 'done' || p.state === 'failed');
  }

  function renderPages(): void {
    const pack = st.get().pack;
    if (!pack) return;
    replace(
      pages,
      ...selectedPages(pack).map((p) =>
        h(
          'li',
          { class: `pack-state state-${p.state}` },
          h('span', { class: `pill pill-pack-${p.state}` }, STATE_TEXT[p.state]),
          ' ',
          p.candidate.title,
          p.error ? h('small', { class: 'bad' }, ` - ${p.error}`) : null,
        ),
      ),
    );
  }

  function showProgress(label: string, p: { done: number; total: number; currentTitle?: string; message?: string }): void {
    line.textContent = `${label} ${p.done} / ${p.total}${p.currentTitle ? ` - ${p.currentTitle}` : ''}${p.message ? ` (${p.message})` : ''}`;
    bar.value = p.total ? p.done / p.total : 0;
  }

  function showErrors(list: PackProgress['errors']): void {
    replace(errors, ...list.map((e) => h('li', null, `${e.title}: ${e.error}`)));
  }

  /** Copy the live page state from fetch progress into the store. */
  function livePage(pack: Pack, p: PackProgress, lastDone: number): Pack {
    if (!p.currentTitle) return pack;
    const failed = p.errors.some((e) => e.title === p.currentTitle);
    const state: PackPage['state'] = p.done > lastDone ? (failed ? 'failed' : 'done') : 'fetching';
    const err = p.errors.find((e) => e.title === p.currentTitle)?.error;
    return {
      ...pack,
      pages: pack.pages.map((pg) =>
        pg.candidate.title === p.currentTitle && pg.candidate.selected ? { ...pg, state, error: err } : pg,
      ),
    };
  }

  async function run(): Promise<void> {
    const start = st.get().pack;
    if (!start) return;
    if (!start.pages.some((p) => p.candidate.selected)) {
      toast('Select at least one page in step 3.', 'warn');
      return;
    }
    ctrl = new AbortController();
    const signal = ctrl.signal;
    btnRun.disabled = true;
    btnCancel.disabled = false;
    replace(errors);
    try {
      // 1. Pages.
      let lastDone = 0;
      const fetched = await fetchPackPages(
        start,
        packExtractOptions(st.get().settings, start),
        (p) => {
          const cur = st.get().pack ?? start;
          st.set({ pack: livePage(cur, p, lastDone) });
          lastDone = p.done;
          showProgress('Pages', p);
          showErrors(p.errors);
          renderPages();
        },
        signal,
      );
      st.set({ pack: fetched });
      renderPages();
      const stats = packStats(fetched);
      if (!stats.articles) throw new Error('No page could be fetched. Look at the errors, then try again.');

      // 2. Images.
      let images = new Map<string, Blob>();
      if (st.get().settings.build.includeImages && fetched.limits.maxImagesPerArticle > 0) {
        images = await fetchPackImages(
          fetched,
          (done, total, title) => showProgress('Images: page', { done, total, currentTitle: title }),
          signal,
        );
      }

      // 3. EPUB.
      if (signal.aborted) throw new DOMException('Cancelled', 'AbortError');
      const file = await buildPackFile(fetched, images, st.get().packAuthor ?? '', (p) => {
        showProgress('Building', p);
        if (p.phase === 'complete' && p.message) toast(p.message, 'info');
      });
      await setBuiltPack(fetched, file, images);
      line.textContent = `Built ${file.name} (${formatBytes(file.blob.size)}), ${stats.articles} articles, ${images.size} images.`;
      bar.value = 1;
      hooks.done();
    } catch (err) {
      if (isAbort(err)) {
        line.textContent = 'Cancelled. Fetched pages are kept. Click "Build pack" to continue.';
      } else {
        line.textContent = `Build failed: ${errorMessage(err)}`;
        toast(`Pack build failed: ${errorMessage(err)}`, 'error');
      }
    } finally {
      ctrl = undefined;
      btnRun.disabled = false;
      btnCancel.disabled = true;
      renderPages();
    }
  }

  btnRun.addEventListener('click', () => void run());
  btnCancel.addEventListener('click', () => ctrl?.abort());
  renderPages();
  return h(
    'div',
    { class: 'pack-step' },
    h('p', { class: 'hint' }, 'The app fetches each page from Wikipedia (or the site), one at a time, then the images, then makes one EPUB.'),
    h('div', { class: 'btn-row' }, btnRun, btnCancel),
    line,
    bar,
    errors,
    pages,
  );
}

export function renderDoneStep(onRestart: () => void, onRebuild: () => void): HTMLElement {
  const st = store();
  const { pack, packOutput } = st.get();
  if (!pack || !packOutput) return h('p', { class: 'hint' }, 'Build the pack first (step 4).');
  const s = packStats(pack);
  const failed = pack.pages.filter((p) => p.state === 'failed');
  return h(
    'div',
    { class: 'pack-step' },
    h('p', { class: 'ok' }, `Pack ready: ${packOutput.file.name} (${formatBytes(packOutput.file.blob.size)}).`),
    h(
      'p',
      null,
      `${s.articles} articles · ${s.words.toLocaleString()} words · ~${packOutput.file.estimatedPages ?? s.pages} pages on X4 Pro · ${packOutput.images.size} images`,
    ),
    failed.length ? h('p', { class: 'bad' }, `${failed.length} page(s) failed. The EPUB lists them under "Missing linked articles".`) : null,
    h(
      'p',
      { class: 'hint' },
      'The pack is now the output. Edit the title, author and filename in the Metadata panel. Use Send, Download, Write to USB folder or Save ZIP on the right.',
    ),
    h(
      'div',
      { class: 'btn-row' },
      h('button', {
        type: 'button',
        class: 'btn primary',
        onclick: async () => {
          try {
            const f = await currentPackFile();
            if (f) downloadFile(f);
          } catch (err) {
            toast(`Download failed: ${errorMessage(err)}`, 'error');
          }
        },
      }, 'Download EPUB'),
      h('button', { type: 'button', class: 'btn', onclick: onRebuild }, 'Change pages'),
      h('button', { type: 'button', class: 'btn', onclick: onRestart }, 'New pack'),
    ),
  );
}
