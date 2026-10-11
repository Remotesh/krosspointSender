/**
 * Device setup wizard. Four steps:
 * 1 Add Device (with the transfer method), 2 Test Connection,
 * 3 Choose Destination Folder (folder browser), 4 Send Test EPUB.
 * Each step saves the profile. Each step shows the limitations of the
 * chosen method for this origin.
 */
import type { DeviceProfile, FirmwareType, OutputFormat, TransferMethod } from '../types';
import { TransferError } from '../types';
import { getTransport, listTransports } from '../transports';
import { renderFolderBrowser } from './folderBrowser';
import { defaultProfile, deleteProfile, listProfiles, saveProfile, validateProfile } from '../deviceProfiles';
import { sendTestEpub } from '../epub';
import { createDrawer, errorMessage, field, h, replace, select, type Drawer } from './dom';
import { asTransferError, persistSettings, sendFile } from './actions';
import { METHODS, renderExplain } from './devicePanel';
import { store } from './state';
import { toast } from './toast';

type Draft = Omit<DeviceProfile, 'id' | 'createdAt' | 'updatedAt'> & Partial<Pick<DeviceProfile, 'id' | 'createdAt' | 'updatedAt'>>;

const STEPS = ['Add Device', 'Test Connection', 'Choose Destination Folder', 'Send Test EPUB'];

export interface DeviceWizard extends Drawer {
  start(profileId?: string): void;
}

export function createDeviceWizard(): DeviceWizard {
  const st = store();
  const drawer = createDrawer('Devices');
  let step = 0;
  let draft: Draft = defaultProfile('crosspoint');

  function methodLabel(m: TransferMethod): string {
    return METHODS.find(([v]) => v === m)?.[1] ?? m;
  }

  /** The limitations box for the chosen method. */
  function limitsBox(): HTMLElement {
    const t = getTransport(draft.method);
    const lim = t.limitations();
    return h(
      'div',
      { class: 'limits-box' },
      h('strong', null, `${methodLabel(draft.method)} from ${location.origin || 'this page'}`),
      !t.isSupported() ? h('p', { class: 'bad' }, 'This browser does not support this method.') : null,
      lim.length ? h('ul', null, ...lim.map((l) => h('li', null, l))) : h('p', { class: 'hint' }, 'No known limits.'),
    );
  }

  async function persist(): Promise<DeviceProfile> {
    const saved = await saveProfile({ ...draft });
    draft = saved;
    const profiles = await listProfiles();
    st.set({ profiles, selectedProfileId: saved.id });
    st.updateSettings((s) => ({ ...s, defaultDeviceId: saved.id }));
    void persistSettings();
    return saved;
  }

  function nav(next?: { label: string; run: () => Promise<boolean> | boolean }): HTMLElement {
    const back = h('button', { type: 'button', class: 'btn', disabled: step === 0, onclick: () => go(step - 1) }, 'Back');
    const nextBtn = next
      ? h('button', {
          type: 'button',
          class: 'btn primary',
          onclick: async (e: Event) => {
            const b = e.currentTarget as HTMLButtonElement;
            b.disabled = true;
            try {
              if (await next.run()) go(step + 1);
            } catch (err) {
              toast(errorMessage(err), 'error');
            } finally {
              b.disabled = false;
            }
          },
        }, next.label)
      : h('button', { type: 'button', class: 'btn primary', onclick: () => drawer.close() }, 'Finish');
    return h('div', { class: 'btn-row wizard-nav' }, back, nextBtn);
  }

  function stepAdd(): HTMLElement {
    const name = h('input', { type: 'text', value: draft.name, required: true });
    const firmware = select<FirmwareType>(
      [['crosspoint', 'CrossPoint'], ['kreader', 'K-Reader (future)'], ['other', 'Other']],
      draft.firmware,
      (v) => {
        const d = defaultProfile(v);
        draft = { ...draft, firmware: v, port: d.port, method: d.method };
        port.value = String(d.port);
        for (const r of Array.from(method.querySelectorAll<HTMLInputElement>('input[type=radio]'))) r.checked = r.value === d.method;
        replace(limitsHost, limitsBox());
      },
    );
    const host = h('input', { type: 'text', value: draft.host, placeholder: 'crosspoint.local or 192.168.x.x' });
    const port = h('input', { type: 'number', min: 1, max: 65535, value: String(draft.port) });
    const folder = h('input', { type: 'text', value: draft.targetFolder });
    const user = h('input', { type: 'text', value: draft.username ?? '', autocomplete: 'off' });
    const pass = h('input', { type: 'password', value: draft.password ?? '', autocomplete: 'off' });
    const fmt = select<OutputFormat>(
      [['epub', 'EPUB'], ['txt', 'TXT'], ['html', 'HTML'], ['md', 'Markdown'], ['zip', 'ZIP']],
      draft.defaultFormat,
      (v) => (draft.defaultFormat = v),
    );
    const notes = h('textarea', { rows: 2, value: draft.notes ?? '' });
    const limitsHost = h('div');
    const method = h(
      'div',
      { class: 'method-list', role: 'radiogroup', 'aria-label': 'Transfer method' },
      ...listTransports().map((t) => {
        const radio = h('input', { type: 'radio', name: 'wiz-method', value: t.method, checked: t.method === draft.method });
        radio.addEventListener('change', () => {
          draft.method = t.method;
          replace(limitsHost, limitsBox());
        });
        return h(
          'label',
          { class: `method${t.isSupported() ? '' : ' unsupported'}` },
          radio,
          ' ',
          methodLabel(t.method),
          t.isSupported() ? '' : ' (not supported here)',
        );
      }),
    );
    replace(limitsHost, limitsBox());
    const problems = h('ul', { class: 'bad', 'aria-live': 'polite' });
    const read = (): void => {
      draft = {
        ...draft,
        name: name.value.trim(),
        host: host.value.trim(),
        port: Number(port.value) || 80,
        targetFolder: folder.value.trim() || '/',
        username: user.value || undefined,
        password: pass.value || undefined,
        notes: notes.value || undefined,
      };
    };
    const del = draft.id
      ? h('button', {
          type: 'button',
          class: 'btn danger small',
          onclick: async () => {
            if (!draft.id || !confirm(`Delete device "${draft.name}"?`)) return;
            await deleteProfile(draft.id);
            const profiles = await listProfiles();
            st.set({ profiles, selectedProfileId: profiles[0]?.id });
            drawer.close();
          },
        }, 'Delete device')
      : null;
    return h(
      'div',
      null,
      h('p', { class: 'hint' }, 'On the X4 Pro open File Transfer. The screen shows the IP address. Use it as the host.'),
      field('Name', name),
      field('Firmware', firmware),
      field('Host', host, 'IP or name. Hotspot mode uses 192.168.4.1.'),
      field('Port', port, 'CrossPoint uses 80 (HTTP, WebDAV) and 81 (WebSocket).'),
      field('Target folder', folder, 'Default /Books/K-rosspoint/. You can browse the device in step 3. A missing folder is created on send.'),
      field('Username (optional)', user),
      field('Password (optional)', pass, 'Stored in this browser only.'),
      field('Default format', fmt),
      field('Notes', notes),
      h('fieldset', null, h('legend', null, 'Transfer method'), h('p', { class: 'hint' }, 'CrossPoint HTTP upload is the default. WebDAV overwrites files. WebSocket shows real progress.'), method, limitsHost),
      problems,
      del,
      nav({
        label: 'Save and continue',
        run: async () => {
          read();
          const errs = validateProfile(draft);
          replace(problems, ...errs.map((e) => h('li', null, e)));
          if (errs.length) return false;
          await persist();
          toast(`Saved device "${draft.name}".`, 'ok');
          return true;
        },
      }),
    );
  }

  function stepTest(): HTMLElement {
    const out = h('div', { 'aria-live': 'polite' });
    const run = h('button', {
      type: 'button',
      class: 'btn primary',
      onclick: async () => {
        replace(out, h('p', null, 'Testing...'));
        try {
          const res = await getTransport(draft.method).testConnection(draft as DeviceProfile);
          if (!res.ok) throw res.error ?? new TransferError('unknown', 'The test failed.');
          replace(
            out,
            h('p', { class: 'ok' }, `Connected in ${res.latencyMs ?? '?'} ms.`),
            res.info ? h('pre', { class: 'code' }, JSON.stringify(res.info, null, 2)) : null,
          );
        } catch (err) {
          replace(out, renderExplain(asTransferError(err)));
        }
      },
    }, 'Test Connection');
    return h(
      'div',
      null,
      h('p', { class: 'hint' }, 'On the X4 Pro open File Transfer first. The device must stay on that screen.'),
      limitsBox(),
      run,
      out,
      nav({
        label: 'Next',
        run: async () => {
          await persist();
          return true;
        },
      }),
    );
  }

  function stepFolder(): HTMLElement {
    const chosen = h('p', { class: 'ok', 'aria-live': 'polite' }, `Destination: ${draft.targetFolder}`);
    const browser = renderFolderBrowser(draft as DeviceProfile, async (path) => {
      draft = { ...draft, targetFolder: path };
      await persist();
      chosen.textContent = `Destination: ${path} (saved)`;
      toast(`Files go to ${path}.`, 'ok');
    });
    return h(
      'div',
      null,
      h('p', { class: 'hint' }, 'Choose where the EPUB files go on the SD card. Default: /Books/K-rosspoint/.'),
      chosen,
      browser,
      nav({
        label: 'Next',
        run: async () => {
          await persist();
          return true;
        },
      }),
    );
  }

  function stepSend(): HTMLElement {
    const out = h('div', { 'aria-live': 'polite' });
    const run = h('button', {
      type: 'button',
      class: 'btn primary',
      onclick: async () => {
        replace(out, h('p', null, 'Sending krosspoint-test.epub...'));
        try {
          const file = await sendTestEpub();
          const res = await sendFile(draft as DeviceProfile, file);
          replace(out, h('p', { class: 'ok' }, `Sent. Open ${res.remotePath ?? draft.targetFolder} on the device.`));
        } catch (err) {
          replace(out, renderExplain(asTransferError(err)));
        }
      },
    }, 'Send Test EPUB');
    return h(
      'div',
      null,
      limitsBox(),
      h('p', { class: 'hint' }, 'This sends a tiny EPUB. Check that it opens on the reader.'),
      run,
      out,
      nav(),
    );
  }

  function go(n: number): void {
    step = Math.max(0, Math.min(STEPS.length - 1, n));
    const steps = h(
      'ol',
      { class: 'wizard-steps' },
      ...STEPS.map((s, i) =>
        h('li', { class: i === step ? 'current' : i < step ? 'done' : '', 'aria-current': i === step ? 'step' : false }, s),
      ),
    );
    const body = [stepAdd, stepTest, stepFolder, stepSend][step]();
    replace(drawer.body, steps, h('h3', null, `Step ${step + 1}: ${STEPS[step]}`), body);
    drawer.body.querySelector<HTMLElement>('input, select, button.primary')?.focus();
  }

  return Object.assign(drawer, {
    start(profileId?: string) {
      const p = profileId ? st.get().profiles.find((x) => x.id === profileId) : undefined;
      draft = p ? { ...p } : { ...defaultProfile('crosspoint') };
      if (!p && !draft.name) draft.name = 'X4 Pro';
      drawer.open();
      go(0);
    },
  });
}
