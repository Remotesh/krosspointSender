/**
 * Output panel. Format, build options and export buttons:
 * Download, Write to USB folder, Save ZIP. Every export saves the
 * article to the library.
 */
import type { OutputFile, OutputFormat } from '../types';
import { getTransport } from '../transports';
import { checkbox, errorMessage, field, formatBytes, h, replace, select } from './dom';
import { buildOutput, downloadFile, fallbackProfile, persistSettings, saveToLibrary, sendFile } from './actions';
import { hasOutput, store } from './state';
import { currentPackFile, packZip } from './packOutput';
import { toast } from './toast';

export const FORMATS: Array<[OutputFormat, string]> = [
  ['epub', 'EPUB (best for X4 Pro)'],
  ['txt', 'TXT (plain text)'],
  ['html', 'HTML'],
  ['md', 'Markdown'],
  ['zip', 'ZIP (EPUB + HTML + TXT + MD)'],
];

/** Build the current article in a format and save it to the library. */
export async function exportCurrent(format: OutputFormat): Promise<OutputFile | undefined> {
  const { article, settings, mode, pack } = store().get();
  // Pack mode: the pack EPUB is the output. ZIP adds one HTML file per article.
  if (mode === 'pack') {
    const file = await currentPackFile();
    if (!file || !pack) return undefined;
    return format === 'zip' ? packZip(file, pack) : file;
  }
  if (!article) {
    toast('Load an article first.', 'warn');
    return undefined;
  }
  const { file, images } = await buildOutput(article, format, settings);
  await saveToLibrary(article, file, images);
  return file;
}

export function mountOutputPanel(root: HTMLElement): void {
  const st = store();
  const status = h('div', { class: 'export-status', 'aria-live': 'polite' });

  const fmt = select(FORMATS, st.get().format, (v) => st.set({ format: v }));
  const setBuild = (key: 'includeCover' | 'includeSourceNote' | 'includeImages') => (v: boolean) => {
    st.updateSettings((s) => ({ ...s, build: { ...s.build, [key]: v } }));
    void persistSettings();
  };
  const b = st.get().settings.build;
  const cover = checkbox('Include cover page', b.includeCover, setBuild('includeCover'));
  const note = checkbox('Include source note', b.includeSourceNote, setBuild('includeSourceNote'));
  const imgs = checkbox('Include images', b.includeImages, setBuild('includeImages'));

  const btnDownload = h('button', { type: 'button', class: 'btn primary' }, 'Download');
  const btnUsb = h('button', { type: 'button', class: 'btn' }, 'Write to USB folder');
  const btnZip = h('button', { type: 'button', class: 'btn' }, 'Save ZIP');
  const buttons = [btnDownload, btnUsb, btnZip];

  async function run(label: string, fn: () => Promise<void>): Promise<void> {
    for (const x of buttons) x.disabled = true;
    status.textContent = `${label}...`;
    try {
      await fn();
    } catch (err) {
      status.textContent = `${label} failed: ${errorMessage(err)}`;
      toast(`${label} failed: ${errorMessage(err)}`, 'error');
    } finally {
      for (const x of buttons) x.disabled = !hasOutput(st.get());
    }
  }

  btnDownload.addEventListener('click', () =>
    void run('Building', async () => {
      const file = await exportCurrent(st.get().format);
      if (!file) return;
      downloadFile(file);
      status.textContent = `Downloaded ${file.name} (${formatBytes(file.blob.size)})`;
    }),
  );
  btnZip.addEventListener('click', () =>
    void run('Building ZIP', async () => {
      const file = await exportCurrent('zip');
      if (!file) return;
      downloadFile(file);
      status.textContent = `Downloaded ${file.name} (${formatBytes(file.blob.size)})`;
    }),
  );
  btnUsb.addEventListener('click', () =>
    void run('Writing to USB', async () => {
      const usb = getTransport('usb');
      if (!usb.isSupported()) {
        toast(usb.limitations().join(' ') || 'This browser cannot write to a folder. Use Download.', 'warn');
        status.textContent = 'USB folder write is not supported in this browser.';
        return;
      }
      const file = await exportCurrent(st.get().format);
      if (!file) return;
      const res = await sendFile(fallbackProfile('usb'), file, st.get().mode === 'pack' ? undefined : st.get().article?.id);
      status.textContent = `Wrote ${res.remotePath ?? file.name} (${formatBytes(res.bytes || file.blob.size)})`;
      toast('File written to the USB folder.', 'ok');
    }),
  );

  const usbNote = h(
    'small',
    { class: 'hint' },
    'USB: put the X4 Pro in USB Drive mode. Then pick the SD card root. Chrome and Edge only.',
  );

  const packNote = h('small', { class: 'hint', hidden: true }, 'Pack mode: Download, Send and USB use the pack EPUB. Save ZIP adds one HTML file per article.');
  replace(
    root,
    h('h2', null, 'Output'),
    packNote,
    field('Format', fmt),
    h('div', { class: 'check-col' }, cover, note, imgs),
    h('div', { class: 'btn-row' }, ...buttons),
    usbNote,
    status,
  );

  st.subscribe((s) => {
    fmt.value = s.format;
    packNote.hidden = s.mode !== 'pack';
    fmt.disabled = s.mode === 'pack';
    for (const x of buttons) x.disabled = !hasOutput(s);
  }, ['format', 'article', 'mode', 'packOutput']);
  st.subscribe((s) => {
    (cover.querySelector('input') as HTMLInputElement).checked = s.settings.build.includeCover;
    (note.querySelector('input') as HTMLInputElement).checked = s.settings.build.includeSourceNote;
    (imgs.querySelector('input') as HTMLInputElement).checked = s.settings.build.includeImages;
  }, ['settings']);
  for (const x of buttons) x.disabled = !hasOutput(st.get());
}
