/**
 * K-rosspointSender boot. Load settings and device profiles, read a
 * bookmarklet payload from the hash, mount the panels, and wire
 * drag-and-drop, paste and keyboard shortcuts.
 */
import './ui/style.css';
import type { AppSettings, DeviceProfile, InputKind } from './types';
import { loadSettings, DEFAULT_SETTINGS } from './settings';
import { listProfiles } from './deviceProfiles';
import { detectInputKind, readPayloadFromHash } from './extractors';
import { createStore, setStore } from './ui/state';
import { h, replace } from './ui/dom';
import { mountSourcePanel, type SourcePanel } from './ui/sourcePanel';
import { mountPreviewPanel } from './ui/previewPanel';
import { mountMetadataPanel } from './ui/metadataPanel';
import { mountOutputPanel } from './ui/outputPanel';
import { mountDevicePanel } from './ui/devicePanel';
import { createLibraryDrawer } from './ui/libraryDrawer';
import { createSettingsDrawer } from './ui/settingsDrawer';
import { createDeviceWizard } from './ui/deviceWizard';
import { runExtract } from './ui/actions';
import { toast } from './ui/toast';

async function boot(): Promise<void> {
  let settings: AppSettings;
  try {
    settings = await loadSettings();
  } catch (err) {
    console.warn('Settings did not load. Defaults are used.', err);
    settings = structuredClone(DEFAULT_SETTINGS) as AppSettings;
  }
  let profiles: DeviceProfile[] = [];
  try {
    profiles = await listProfiles();
  } catch (err) {
    console.warn('Device profiles did not load.', err);
  }
  const selected =
    profiles.find((p) => p.id === settings.defaultDeviceId) ?? (profiles.length ? profiles[0] : undefined);

  const store = createStore({
    settings,
    profiles,
    selectedProfileId: selected?.id,
    format: selected?.defaultFormat ?? 'epub',
    transfer: { state: 'idle' },
    showImages: settings.build.includeImages,
    zoom: 1,
    busy: false,
    mode: 'article',
  });
  setStore(store);

  // ── Layout ──
  const app = document.getElementById('app')!;
  const sourceEl = h('section', { class: 'panel panel-source', 'aria-label': 'Source' });
  const previewEl = h('section', { class: 'panel panel-preview', 'aria-label': 'Preview' });
  const metaEl = h('section', { class: 'panel', 'aria-label': 'Metadata' });
  const outputEl = h('section', { class: 'panel', 'aria-label': 'Output' });
  const deviceEl = h('section', { class: 'panel', 'aria-label': 'Device' });

  const library = createLibraryDrawer();
  const settingsDrawer = createSettingsDrawer();
  const wizard = createDeviceWizard();

  const topbar = h(
    'header',
    { class: 'topbar' },
    h('h1', null, h('span', { class: 'brand-k' }, 'K'), '-rosspointSender'),
    h('span', { class: 'tagline' }, 'articles to EPUB for the X4 Pro · local only'),
    h(
      'nav',
      { class: 'top-actions', 'aria-label': 'Main' },
      h('button', { type: 'button', class: 'btn', onclick: () => library.open() }, 'Library'),
      h('button', { type: 'button', class: 'btn', onclick: () => settingsDrawer.open() }, 'Settings'),
      h('button', { type: 'button', class: 'btn', onclick: () => wizard.start(store.get().selectedProfileId) }, 'Devices'),
    ),
  );
  replace(
    app,
    topbar,
    h('main', { class: 'layout' }, sourceEl, previewEl, h('div', { class: 'col-right' }, metaEl, outputEl, deviceEl)),
  );

  const source: SourcePanel = mountSourcePanel(sourceEl);
  mountPreviewPanel(previewEl);
  mountMetadataPanel(metaEl);
  mountOutputPanel(outputEl);
  mountDevicePanel(deviceEl, { openWizard: (id) => wizard.start(id) });

  wireDragDrop(sourceEl, source);
  wirePaste(source);
  wireKeys(source);

  // ── Bookmarklet payload in the hash ──
  const hashPayload = (): void => {
    if (!location.hash.includes('payload=')) return;
    const payload = readPayloadFromHash(location.hash);
    history.replaceState(null, '', location.pathname + location.search);
    if (payload) {
      source.selectTab('bookmarklet');
      source.loadPayload(payload);
    } else {
      toast('The page link has a payload, but it is not valid.', 'error');
    }
  };
  hashPayload();
  window.addEventListener('hashchange', hashPayload);

  if (!profiles.length) {
    toast('No device yet. Click Devices to add your X4 Pro.', 'info', 6000);
  }
}

/** Drop files or text on the source panel. */
function wireDragDrop(zone: HTMLElement, source: SourcePanel): void {
  let depth = 0;
  zone.addEventListener('dragenter', (e) => {
    e.preventDefault();
    depth++;
    zone.classList.add('drop-active');
  });
  zone.addEventListener('dragleave', () => {
    depth = Math.max(0, depth - 1);
    if (!depth) zone.classList.remove('drop-active');
  });
  zone.addEventListener('dragover', (e) => {
    e.preventDefault();
    if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
  });
  zone.addEventListener('drop', (e) => {
    e.preventDefault();
    depth = 0;
    zone.classList.remove('drop-active');
    const dt = e.dataTransfer;
    if (!dt) return;
    if (dt.files.length) {
      source.loadFiles(dt.files);
      return;
    }
    const html = dt.getData('text/html');
    const text = dt.getData('text/uri-list') || dt.getData('text/plain');
    loadText(source, html, text);
  });
  // Stop the browser from opening a file dropped outside the zone.
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => e.preventDefault());
}

/** Load pasted or dropped text. A URL is fetched. HTML is extracted. */
function loadText(source: SourcePanel, html: string, text: string): void {
  const t = text.trim();
  if (!html && !t) return;
  const kind: InputKind = html ? 'html' : detectInputKind(t);
  if (kind === 'url' || kind === 'wikipedia') {
    source.fetchUrl(t.split(/\s+/)[0]);
    return;
  }
  void runExtract({ kind, content: html || t });
}

/** Paste outside a text field: treat the clipboard as an input. */
function wirePaste(source: SourcePanel): void {
  document.addEventListener('paste', (e) => {
    const target = e.target as HTMLElement | null;
    if (target && (target.closest('input, textarea, select, [contenteditable]') || target.closest('.drawer'))) return;
    const cd = e.clipboardData;
    if (!cd) return;
    if (cd.files.length) {
      e.preventDefault();
      source.loadFiles(cd.files);
      return;
    }
    const html = cd.getData('text/html');
    const text = cd.getData('text/plain');
    if (!html && !text.trim()) return;
    e.preventDefault();
    loadText(source, html, text);
  });
}

/** Ctrl/Cmd+Enter runs the open source tab. */
function wireKeys(source: SourcePanel): void {
  document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
      if ((e.target as HTMLElement | null)?.closest('.drawer')) return;
      e.preventDefault();
      source.convert();
    }
  });
}

boot().catch((err) => {
  console.error(err);
  const app = document.getElementById('app');
  if (app) {
    app.textContent = `K-rosspointSender could not start: ${err instanceof Error ? err.message : String(err)}`;
  }
});
