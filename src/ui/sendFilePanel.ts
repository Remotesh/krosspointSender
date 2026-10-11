/**
 * "Send a file" section of the Device panel. It sends any file from disk
 * (for example a CrossPoint firmware.bin) to the reader's SD card with the
 * selected device's transport. Nothing is converted. Progress goes to the
 * shared transfer status line of the Device panel (store key 'transfer').
 *
 * Other code opens the section with files through the window event
 * SEND_FILE_OFFER_EVENT (detail: { files: File[] }). The source panel uses
 * it for a dropped file that is not an article.
 */
import type { TransferProgress } from '../types';
import { TransferError } from '../types';
import { explainBlock, getTransport, normalizeFolder } from '../transports';
import { markConnected } from '../deviceProfiles';
import { logTransfer } from '../storage';
import { FIRMWARE_HINT, isFirmwareFile, sendRawFile } from '../send';
import { errorMessage, field, formatBytes, h, replace } from './dom';
import { asTransferError } from './actions';
import { store } from './state';
import { toast } from './toast';

export const SEND_FILE_OFFER_EVENT = 'krosspoint:send-file-offer';

/** Open the "Send a file" section with these files. */
export function offerSendFile(files: File[]): void {
  window.dispatchEvent(new CustomEvent(SEND_FILE_OFFER_EVENT, { detail: { files } }));
}

export interface SendFileSection {
  el: HTMLElement;
  setFiles(files: File[]): void;
}

export function mountSendFileSection(): SendFileSection {
  const st = store();
  let selected: File[] = [];
  let folderTouched = false;
  let controller: AbortController | undefined;

  const fileInput = h('input', { type: 'file', multiple: true });
  const fileList = h('ul', { class: 'limits send-file-list' });
  const folderInput = h('input', { type: 'text', spellcheck: 'false', autocomplete: 'off', placeholder: '/' });
  const btnRoot = h('button', { type: 'button', class: 'btn small' }, '/');
  const btnDefault = h('button', { type: 'button', class: 'btn small' }, 'Device default');
  const nameInput = h('input', { type: 'text', spellcheck: 'false', autocomplete: 'off' });
  const nameHint = h('small', { class: 'hint' });
  const btnSend = h('button', { type: 'button', class: 'btn primary' }, 'Send file');
  const btnCancel = h('button', { type: 'button', class: 'btn', hidden: true }, 'Cancel');
  const results = h('ul', { class: 'limits send-file-results', 'aria-live': 'polite' });
  const hint = h('p', { class: 'hint send-file-hint', hidden: true }, FIRMWARE_HINT);
  const explainHost = h('div');
  const firmwareTip = h('small', { class: 'hint', hidden: true }, 'Firmware: send it to / (the card root).');

  const nameField = field('File name on the reader', nameInput);
  nameField.appendChild(nameHint);

  const details = h(
    'details',
    { class: 'send-file' },
    h('summary', null, 'Send a file'),
    h(
      'p',
      { class: 'hint' },
      'Put any file from your disk on the reader, as it is. For example a CrossPoint firmware.bin: send it to /, then install it on the reader.',
    ),
    field('Files', fileInput, 'Any type. Choose more than one to send them in turn.'),
    fileList,
    field('Destination folder', folderInput),
    h('div', { class: 'btn-row tight' }, btnRoot, btnDefault, firmwareTip),
    nameField,
    h('div', { class: 'btn-row' }, btnSend, btnCancel),
    results,
    hint,
    explainHost,
  );

  function deviceFolder(): string {
    return normalizeFolder(st.selectedProfile()?.targetFolder ?? '/');
  }

  function syncFolder(): void {
    const def = deviceFolder();
    btnDefault.textContent = `Device default (${def})`;
    if (!folderTouched) folderInput.value = def;
  }

  function setFiles(files: File[]): void {
    selected = files;
    replace(results);
    replace(explainHost);
    hint.hidden = true;
    replace(fileList, ...files.map((f) => h('li', null, `${f.name} (${formatBytes(f.size)})`)));
    if (files.length === 1) {
      nameInput.disabled = false;
      nameInput.value = files[0].name;
      nameHint.textContent = 'You can change the name.';
    } else {
      nameInput.disabled = true;
      nameInput.value = '';
      nameHint.textContent = files.length > 1 ? 'Each file keeps its own name.' : '';
    }
    firmwareTip.hidden = !files.some((f) => isFirmwareFile(f.name));
    updateButtons();
  }

  function updateButtons(): void {
    const busy = !!controller;
    btnSend.disabled = busy || !selected.length || !st.selectedProfile();
    btnCancel.hidden = !busy;
    fileInput.disabled = busy;
  }

  function resultLine(ok: boolean, text: string): HTMLElement {
    return h('li', { class: ok ? '' : 'bad' }, text);
  }

  async function sendAll(): Promise<void> {
    const p = st.selectedProfile();
    if (!p) {
      toast('Add or choose a device first.', 'warn');
      return;
    }
    if (!selected.length) return;
    const files = [...selected];
    const folder = folderInput.value.trim() || '/';
    const transport = getTransport(p.method);
    controller = new AbortController();
    const signal = controller.signal;
    replace(results);
    replace(explainHost);
    hint.hidden = true;
    updateButtons();
    let okCount = 0;
    let firmwareSent = false;
    try {
      for (let i = 0; i < files.length; i++) {
        const f = files[i];
        const prefix = files.length > 1 ? `[${i + 1}/${files.length}] ` : '';
        const report = (pr: TransferProgress): void =>
          st.set({ transfer: { ...pr, message: pr.message ? `${prefix}${pr.message}` : prefix.trim() || undefined } });
        try {
          const out = await sendRawFile({
            profile: p,
            file: f,
            name: files.length === 1 ? nameInput.value : undefined,
            folder,
            transport,
            onProgress: report,
            signal,
            log: logTransfer,
          });
          okCount++;
          if (out.hint) firmwareSent = true;
          results.appendChild(resultLine(true, `Sent ${out.file.name} as ${out.remotePath} (${formatBytes(out.file.blob.size)})`));
          if (out.warning) results.appendChild(h('li', { class: 'warn' }, out.warning));
          if (p.id !== 'adhoc') void markConnected(p.id).catch(() => undefined);
        } catch (err) {
          const te = asTransferError(err);
          results.appendChild(resultLine(false, `${f.name}: ${te.message}`));
          if (te.code === 'cancelled') break;
          if (te.code !== 'too-large' && te.code !== 'exists') {
            replace(explainHost, renderFileExplain(te, p.method));
          }
        }
      }
    } finally {
      controller = undefined;
      updateButtons();
    }
    hint.hidden = !firmwareSent;
    if (okCount === files.length) {
      toast(files.length === 1 ? `Sent ${files[0].name} to ${p.name}.` : `Sent ${okCount} files to ${p.name}.`, 'ok');
    } else if (okCount > 0) {
      toast(`Sent ${okCount} of ${files.length} files.`, 'warn');
    }
  }

  fileInput.addEventListener('change', () => setFiles(Array.from(fileInput.files ?? [])));
  folderInput.addEventListener('input', () => {
    folderTouched = true;
  });
  folderInput.addEventListener('change', () => {
    folderInput.value = normalizeFolder(folderInput.value);
  });
  btnRoot.addEventListener('click', () => {
    folderTouched = true;
    folderInput.value = '/';
  });
  btnDefault.addEventListener('click', () => {
    folderTouched = false;
    syncFolder();
  });
  btnSend.addEventListener('click', () => void sendAll().catch((e) => toast(errorMessage(e), 'error')));
  btnCancel.addEventListener('click', () => controller?.abort());

  window.addEventListener(SEND_FILE_OFFER_EVENT, (e) => {
    const files = (e as CustomEvent<{ files?: File[] }>).detail?.files ?? [];
    if (!files.length) return;
    if (controller) {
      toast('A file send is running. Wait for it to finish.', 'warn');
      return;
    }
    details.open = true;
    setFiles(files);
    details.scrollIntoView?.({ behavior: 'smooth', block: 'nearest' });
    btnSend.focus();
  });

  st.subscribe(() => {
    syncFolder();
    updateButtons();
  }, ['profiles', 'selectedProfileId']);
  syncFolder();
  setFiles([]);

  return { el: details, setFiles };
}

/** The explain box for a failed file send. No fallback buttons: they export the article. */
function renderFileExplain(err: TransferError, method: Parameters<typeof explainBlock>[2]): HTMLElement {
  const info = explainBlock(err, location.origin, method);
  return h(
    'div',
    { class: 'explain', role: 'alert' },
    h('strong', null, info.title),
    info.reasons.length ? h('ul', null, ...info.reasons.map((r) => h('li', null, r))) : null,
    info.fallbacks.length
      ? h('div', null, h('p', null, 'What you can do:'), h('ul', null, ...info.fallbacks.map((f) => h('li', null, f))))
      : null,
  );
}
