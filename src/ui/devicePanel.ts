/**
 * Device panel. Profile select, transfer method, Test Connection,
 * Send Test EPUB, Send, and a status line with the transfer states.
 * On failure it shows explainBlock() and fallback buttons.
 */
import type { DeviceProfile, TransferMethod, TransferProgress, TransferState } from '../types';
import { TransferError } from '../types';
import { downloadBlob, explainBlock, getHelperTransport, getTransport } from '../transports';
import { saveProfile } from '../deviceProfiles';
import { sendTestEpub } from '../epub';
import { errorMessage, field, formatBytes, h, replace } from './dom';
import { asTransferError, downloadFile, fallbackProfile, persistSettings, sendFile } from './actions';
import { exportCurrent } from './outputPanel';
import { hasOutput, store } from './state';
import { recordPackSend } from './packOutput';
import { toast } from './toast';

/**
 * Save this page as one HTML file. The app is a single self-contained file,
 * so the saved copy is the whole app. Opened from disk (file://), a browser
 * does not apply the https mixed-content rule, and the LAN upload works.
 */
function saveOfflineCopy(): void {
  const html = '<!DOCTYPE html>\n' + document.documentElement.outerHTML;
  downloadBlob(new Blob([html], { type: 'text/html' }), 'k-rosspointSender.html');
  toast('Saved. Open the file from your disk; sending to the reader works from there.', 'info');
}

export const METHODS: Array<[TransferMethod, string]> = [
  ['crosspoint-http', 'CrossPoint HTTP upload (default)'],
  ['webdav', 'WebDAV (port 80, overwrites)'],
  ['crosspoint-ws', 'CrossPoint WebSocket (port 81)'],
  ['usb', 'USB folder (File System Access)'],
  ['download', 'Manual download'],
  ['opds', 'OPDS package (ZIP)'],
  ['ble', 'Bluetooth (K-Reader, simulated)'],
];

/** README section that explains the https LAN block. */
export const README_LAN_SECTION = 'README: Sending to CrossPoint > Over Wi-Fi (same LAN)';

const STATE_TEXT: Record<TransferState, string> = {
  idle: 'Idle',
  connecting: 'Connecting',
  uploading: 'Uploading',
  verifying: 'Verifying',
  complete: 'Complete',
  failed: 'Failed',
};

export interface DevicePanelHooks {
  openWizard(profileId?: string): void;
}

/** Make the explain box for a failed transfer, with fallback buttons. */
export function renderExplain(err: TransferError, onDone?: () => void): HTMLElement {
  const info = explainBlock(err, location.origin);
  const st = store();
  const fallbackBtn = (label: string, fn: () => Promise<void> | void): HTMLButtonElement =>
    h('button', {
      type: 'button',
      class: 'btn',
      onclick: async () => {
        try {
          await fn();
          onDone?.();
        } catch (e) {
          toast(errorMessage(e), 'error');
        }
      },
    }, label);
  return h(
    'div',
    { class: 'explain', role: 'alert' },
    h('strong', null, info.title),
    info.reasons.length ? h('ul', null, ...info.reasons.map((r) => h('li', null, r))) : null,
    info.fallbacks.length
      ? h('div', null, h('p', null, 'What you can do:'), h('ul', null, ...info.fallbacks.map((f) => h('li', null, f))))
      : null,
    h(
      'div',
      { class: 'btn-row' },
      fallbackBtn('Download instead', async () => {
        const file = await exportCurrent(st.get().format);
        if (file) downloadFile(file);
      }),
      getTransport('usb').isSupported()
        ? fallbackBtn('Write to USB folder', async () => {
            const file = await exportCurrent(st.get().format);
            if (file) await sendFile(fallbackProfile('usb'), file, st.get().mode === 'pack' ? undefined : st.get().article?.id);
          })
        : null,
      st.get().settings.helperUrl && st.selectedProfile()
        ? fallbackBtn('Send via helper app', async () => {
            const p = st.selectedProfile();
            const file = await exportCurrent(st.get().format);
            if (!p || !file) return;
            st.set({ transfer: { state: 'connecting', message: 'Sending through the helper app' } });
            const res = await getHelperTransport().send(p, file, (pr) => st.set({ transfer: pr }));
            if (!res.ok) throw res.error ?? new TransferError('unknown', 'The helper did not send the file.');
            st.set({ transfer: { state: 'complete', fraction: 1, message: `Sent via helper: ${res.remotePath ?? file.name}` } });
          })
        : null,
      fallbackBtn('Try WebDAV', () => switchMethod('webdav')),
      fallbackBtn('Try HTTP upload', () => switchMethod('crosspoint-http')),
    ),
  );
}

/** Change the method of the selected profile and save it. */
async function switchMethod(method: TransferMethod): Promise<void> {
  const st = store();
  const p = st.selectedProfile();
  if (!p) {
    toast('Add a device first.', 'warn');
    return;
  }
  const saved = await saveProfile({ ...p, method });
  st.set({ profiles: st.get().profiles.map((x) => (x.id === saved.id ? saved : x)) });
  toast(`Method set to ${method}.`, 'info');
}

export function mountDevicePanel(root: HTMLElement, hooks: DevicePanelHooks): void {
  const st = store();
  const profileSel = h('select');
  const methodSel = h('select', null, ...METHODS.map(([v, t]) => h('option', { value: v }, t)));
  const limits = h('ul', { class: 'limits' });
  const pill = h('span', { class: 'pill pill-idle' }, 'Idle');
  const statusText = h('span', { class: 'status-text' });
  const bar = h('progress', { max: 1, value: 0, 'aria-label': 'Transfer progress' });
  const statusLine = h('div', { class: 'status-line', role: 'status', 'aria-live': 'polite' }, pill, statusText);
  const explainHost = h('div');

  const btnTest = h('button', { type: 'button', class: 'btn' }, 'Test Connection');
  const btnTestEpub = h('button', { type: 'button', class: 'btn' }, 'Send Test EPUB');
  const btnSend = h('button', { type: 'button', class: 'btn primary' }, 'Send');
  const btnEdit = h('button', { type: 'button', class: 'btn small' }, 'Edit');
  const btnAdd = h('button', { type: 'button', class: 'btn small' }, 'Add device');

  const banner =
    location.protocol === 'https:'
      ? h(
          'div',
          { class: 'banner', role: 'note' },
          h('p', null, 'LAN upload from an https page is blocked by browsers. Save this page and open it from your disk: sending works from there.'),
          h('button', { type: 'button', class: 'btn small', onclick: saveOfflineCopy }, 'Save an offline copy'),
          h(
            'details',
            null,
            h('summary', null, 'Why, and what to do'),
            h(
              'ul',
              null,
              h('li', null, 'The device serves plain http. An https page may not call http (mixed content).'),
              h('li', null, 'Chrome also blocks public sites from calling private network addresses.'),
              h('li', null, 'Offline copy: this app is one file. Save it, open it from your disk, and the LAN upload works (Chrome, Edge, Firefox). Your library stays in that browser.'),
              h('li', null, 'USB: put the X4 Pro in USB Drive mode and use "Write to USB folder".'),
              h('li', null, 'Desktop app: it sends on its own, and it serves as the helper for this page (Settings > Helper URL).'),
            ),
            h('p', { class: 'hint' }, `More: ${README_LAN_SECTION}.`),
          ),
        )
      : null;

  profileSel.addEventListener('change', () => {
    const id = profileSel.value || undefined;
    st.set({ selectedProfileId: id });
    st.updateSettings((s) => ({ ...s, defaultDeviceId: id }));
    void persistSettings();
  });
  methodSel.addEventListener('change', () => void switchMethod(methodSel.value as TransferMethod).catch((e) => toast(errorMessage(e), 'error')));
  btnEdit.addEventListener('click', () => hooks.openWizard(st.get().selectedProfileId));
  btnAdd.addEventListener('click', () => hooks.openWizard());

  const actionBtns = [btnTest, btnTestEpub, btnSend];
  async function guard(fn: (p: DeviceProfile) => Promise<void>): Promise<void> {
    const p = st.selectedProfile();
    if (!p) {
      toast('Add or choose a device first.', 'warn');
      hooks.openWizard();
      return;
    }
    replace(explainHost);
    for (const b of actionBtns) b.disabled = true;
    try {
      await fn(p);
    } catch (err) {
      const te = asTransferError(err);
      st.set({ transfer: { state: 'failed', message: te.message } });
      replace(explainHost, renderExplain(te, () => replace(explainHost)));
    } finally {
      for (const b of actionBtns) b.disabled = false;
      updateButtons();
    }
  }

  btnTest.addEventListener('click', () =>
    void guard(async (p) => {
      st.set({ transfer: { state: 'connecting', message: `Testing ${p.host}:${p.port}` } });
      const res = await getTransport(p.method).testConnection(p);
      if (!res.ok) throw res.error ?? new TransferError('unknown', 'The test failed.');
      const ver = res.info && typeof res.info.version === 'string' ? ` (firmware ${res.info.version})` : '';
      st.set({
        transfer: { state: 'complete', message: `Connected in ${res.latencyMs ?? '?'} ms${ver}` },
      });
    }),
  );
  btnTestEpub.addEventListener('click', () =>
    void guard(async (p) => {
      const file = await sendTestEpub();
      await sendFile(p, file);
      toast(`Sent ${file.name}. Look in ${p.targetFolder} on the device.`, 'ok');
    }),
  );
  btnSend.addEventListener('click', () =>
    void guard(async (p) => {
      const { format, article, mode } = st.get();
      if (mode === 'pack') {
        st.set({ transfer: { state: 'connecting', message: 'Preparing the pack EPUB' } });
        const file = await exportCurrent('epub');
        if (!file) return;
        try {
          await sendFile(p, file);
          await recordPackSend(p, true);
        } catch (err) {
          await recordPackSend(p, false, err);
          throw err;
        }
        toast(`Sent ${file.name} to ${p.name}.`, 'ok');
        return;
      }
      if (!article) {
        toast('Load an article first.', 'warn');
        return;
      }
      if (p.firmware === 'crosspoint' && (format === 'html' || format === 'md' || format === 'zip')) {
        toast(`CrossPoint cannot open .${format} files. It reads EPUB and TXT.`, 'warn');
      }
      st.set({ transfer: { state: 'connecting', message: 'Building file' } });
      const file = await exportCurrent(format);
      if (!file) return;
      await sendFile(p, file, article.id);
      toast(`Sent ${file.name} to ${p.name}.`, 'ok');
    }),
  );

  replace(
    root,
    h('h2', null, 'Device'),
    banner,
    h('div', { class: 'row' }, field('Device', profileSel), h('div', { class: 'btn-col tight' }, btnEdit, btnAdd)),
    field('Transfer method', methodSel),
    limits,
    h('div', { class: 'btn-row' }, btnTest, btnTestEpub, btnSend),
    statusLine,
    bar,
    explainHost,
  );

  function renderProfiles(): void {
    const { profiles, selectedProfileId } = st.get();
    replace(
      profileSel,
      h('option', { value: '' }, profiles.length ? '(choose a device)' : '(no devices yet)'),
      ...profiles.map((p) => h('option', { value: p.id }, `${p.name} (${p.host})`)),
    );
    profileSel.value = selectedProfileId ?? '';
    const p = st.selectedProfile();
    methodSel.disabled = !p;
    btnEdit.disabled = !p;
    methodSel.value = p?.method ?? 'crosspoint-http';
    const lim = p ? getTransport(p.method).limitations() : [];
    const unsupported = p && !getTransport(p.method).isSupported();
    replace(
      limits,
      unsupported ? h('li', { class: 'bad' }, 'This browser does not support this method.') : null,
      ...lim.map((l) => h('li', null, l)),
    );
    updateButtons();
  }

  function updateButtons(): void {
    const p = st.selectedProfile();
    btnSend.disabled = !p || !hasOutput(st.get());
    btnTest.disabled = !p;
    btnTestEpub.disabled = !p;
  }

  function renderStatus(t: TransferProgress): void {
    pill.className = `pill pill-${t.state}`;
    pill.textContent = STATE_TEXT[t.state];
    let text = t.message ?? '';
    if (t.state === 'uploading' && t.bytesTotal) {
      text = `${formatBytes(t.bytesSent ?? 0)} / ${formatBytes(t.bytesTotal)}${text ? ` - ${text}` : ''}`;
    }
    statusText.textContent = text;
    const busy = t.state === 'connecting' || t.state === 'uploading' || t.state === 'verifying';
    bar.hidden = !busy && t.state !== 'complete';
    if (t.fraction !== undefined) bar.value = t.fraction;
    else if (busy) bar.removeAttribute('value');
    else bar.value = t.state === 'complete' ? 1 : 0;
  }

  st.subscribe(renderProfiles, ['profiles', 'selectedProfileId']);
  st.subscribe(updateButtons, ['article', 'mode', 'packOutput']);
  st.subscribe((s) => renderStatus(s.transfer), ['transfer']);
  renderProfiles();
  renderStatus(st.get().transfer);
}
