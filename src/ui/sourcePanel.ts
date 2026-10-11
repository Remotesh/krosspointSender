/**
 * Source panel (left column). Tabs for each input kind.
 * The fetch tabs show the FetchError reason and fallback buttons on failure.
 */
import type { BookmarkletPayload, InputKind, RawInput } from '../types';
import { detectInputKind, fetchErrorHelp } from '../extractors';
import { errorMessage, field, h, replace } from './dom';
import { fetchAndExtract, runExtract } from './actions';
import { renderBookmarkletPanel } from './bookmarkletPanel';
import { mountPackWizard } from './packWizard';
import { store } from './state';
import { toast } from './toast';
import { offerSendFile } from './sendFilePanel';
import { isArticleFile } from '../send';

export type SourceTab = 'url' | 'wikipedia' | 'html' | 'text' | 'markdown' | 'file' | 'bookmarklet' | 'pack';

const TABS: Array<[SourceTab, string]> = [
  ['url', 'URL'],
  ['wikipedia', 'Wikipedia'],
  ['html', 'Paste HTML'],
  ['text', 'Paste text'],
  ['markdown', 'Markdown'],
  ['file', 'File'],
  ['bookmarklet', 'Bookmarklet'],
  ['pack', '[ pack ]'],
];

export interface SourcePanel {
  selectTab(tab: SourceTab): void;
  /** Run the action of the open tab (Ctrl/Cmd+Enter). */
  convert(): void;
  /** Load a bookmarklet payload. */
  loadPayload(p: BookmarkletPayload): void;
  /** Read dropped or picked files. */
  loadFiles(files: FileList | File[]): void;
  /** Put a URL in the URL tab and fetch it. */
  fetchUrl(url: string): void;
}

/** Map a help line from fetchErrorHelp() to a source tab, when it names one. */
function tabForHelp(line: string): SourceTab | undefined {
  const l = line.toLowerCase();
  if (l.includes('bookmarklet')) return 'bookmarklet';
  if (l.includes('paste html')) return 'html';
  if (l.includes('.html file') || l.includes('drop the')) return 'file';
  if (l.includes('paste text')) return 'text';
  return undefined;
}

/** Read a File into a RawInput. The extension picks the kind. */
export async function fileToInput(file: File): Promise<RawInput> {
  const text = await file.text();
  const ext = file.name.toLowerCase().split('.').pop() ?? '';
  let kind: InputKind;
  if (ext === 'html' || ext === 'htm' || ext === 'xhtml') kind = 'html';
  else if (ext === 'md' || ext === 'markdown') kind = 'markdown';
  else if (ext === 'txt') kind = 'text';
  else kind = detectInputKind(text, file.name);
  if (kind === 'url' || kind === 'wikipedia') {
    // A file with only a URL in it: treat it as text. The user can fetch it from the URL tab.
    kind = 'text';
  }
  return { kind, content: text, fileName: file.name };
}

export function mountSourcePanel(root: HTMLElement): SourcePanel {
  let active: SourceTab = 'url';
  const bodies = new Map<SourceTab, HTMLElement>();
  const tabButtons = new Map<SourceTab, HTMLButtonElement>();
  let controller: AbortController | undefined;

  // ── URL tab ──
  const urlInput = h('input', { type: 'url', placeholder: 'https://example.com/article', autocomplete: 'url' });
  const urlStatus = h('div', { class: 'fetch-status', 'aria-live': 'polite' });
  const urlFail = h('div', { class: 'fetch-fail', hidden: true });
  const fetchBtn = h('button', { type: 'button', class: 'btn primary' }, 'Fetch');

  // ── Wikipedia tab ──
  const wikiInput = h('input', { type: 'text', placeholder: 'Article title or Wikipedia URL' });
  const wikiLang = h('input', { type: 'text', value: 'en', size: 4, maxlength: 12 });
  const wikiStatus = h('div', { class: 'fetch-status', 'aria-live': 'polite' });
  const wikiFail = h('div', { class: 'fetch-fail', hidden: true });

  async function doFetch(raw: string, status: HTMLElement, fail: HTMLElement): Promise<void> {
    if (!raw.trim()) {
      toast('Type a URL first.', 'warn');
      return;
    }
    controller?.abort();
    controller = new AbortController();
    fail.hidden = true;
    status.textContent = 'Fetching...';
    status.className = 'fetch-status busy';
    try {
      const article = await fetchAndExtract(raw, controller.signal);
      status.textContent = article ? `Loaded: ${article.title}` : 'Fetched, but extraction failed.';
      status.className = article ? 'fetch-status ok' : 'fetch-status bad';
    } catch (err) {
      const help = fetchErrorHelp(err);
      const reasons = help.filter((l) => !tabForHelp(l));
      const steps = help.filter((l) => tabForHelp(l));
      status.textContent = `Fetch failed: ${errorMessage(err)}`;
      status.className = 'fetch-status bad';
      replace(
        fail,
        ...reasons.map((r, i) => (i === 0 ? h('strong', null, r) : h('p', null, r))),
        h('p', null, 'Try one of these:'),
        h(
          'div',
          { class: 'btn-col' },
          ...steps.map((line) =>
            h('button', { type: 'button', class: 'btn left', onclick: () => api.selectTab(tabForHelp(line)!) }, line),
          ),
        ),
      );
      fail.hidden = false;
    }
  }

  fetchBtn.addEventListener('click', () => void doFetch(urlInput.value, urlStatus, urlFail));
  urlInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') void doFetch(urlInput.value, urlStatus, urlFail);
  });

  function wikiUrl(): string {
    const v = wikiInput.value.trim();
    if (/^https?:\/\//i.test(v) || /wikipedia\.org/i.test(v)) return v;
    const lang = (wikiLang.value.trim() || 'en').replace(/[^a-z-]/gi, '');
    return `https://${lang}.wikipedia.org/wiki/${encodeURIComponent(v.replace(/ /g, '_'))}`;
  }
  const wikiFetch = (): void => void doFetch(wikiUrl(), wikiStatus, wikiFail);
  wikiInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') wikiFetch();
  });

  bodies.set(
    'url',
    h(
      'div',
      { class: 'tab-body' },
      field('Article URL', urlInput, 'The site must allow cross-origin reads. Many do not.'),
      fetchBtn,
      urlStatus,
      urlFail,
    ),
  );
  bodies.set(
    'wikipedia',
    h(
      'div',
      { class: 'tab-body' },
      field('Wikipedia article', wikiInput),
      field('Language code', wikiLang),
      h('button', { type: 'button', class: 'btn primary', onclick: wikiFetch }, 'Fetch from Wikipedia'),
      wikiStatus,
      wikiFail,
    ),
  );

  // ── Paste tabs ──
  function pasteTab(kind: 'html' | 'text' | 'markdown', label: string, placeholder: string): HTMLTextAreaElement {
    const area = h('textarea', { rows: 14, placeholder, spellcheck: kind !== 'html' });
    const base = kind === 'html' ? h('input', { type: 'url', placeholder: 'https://... (optional)' }) : null;
    const go = (): void => {
      if (!area.value.trim()) {
        toast('The box is empty.', 'warn');
        return;
      }
      void runExtract({ kind, content: area.value, sourceUrl: base?.value.trim() || undefined });
    };
    convertFns.set(kind, go);
    bodies.set(
      kind,
      h(
        'div',
        { class: 'tab-body' },
        field(label, area),
        base ? field('Source URL (for links and images)', base) : null,
        h('button', { type: 'button', class: 'btn primary', onclick: go }, 'Convert'),
        h('small', { class: 'hint' }, 'Shortcut: Ctrl+Enter or Cmd+Enter.'),
      ),
    );
    return area;
  }
  const convertFns = new Map<SourceTab, () => void>();
  convertFns.set('url', () => void doFetch(urlInput.value, urlStatus, urlFail));
  convertFns.set('wikipedia', wikiFetch);
  pasteTab('html', 'Page HTML', '<html>...</html>');
  pasteTab('text', 'Plain text', 'Paste text. A blank line starts a new paragraph.');
  pasteTab('markdown', 'Markdown', '# Title\n\nText...');

  // ── File tab ──
  const fileInput = h('input', {
    type: 'file',
    accept: '.html,.htm,.xhtml,.md,.markdown,.txt,text/html,text/plain,text/markdown',
  });
  fileInput.addEventListener('change', () => {
    if (fileInput.files?.length) api.loadFiles(fileInput.files);
    fileInput.value = '';
  });
  bodies.set(
    'file',
    h(
      'div',
      { class: 'tab-body' },
      field('Choose a file', fileInput, 'HTML, Markdown or TXT. You can also drop a file anywhere on this panel. Other files (for example firmware.bin) go to Device > Send a file.'),
      h('div', { class: 'drop-hint' }, 'Drop a file here'),
    ),
  );

  // ── Bookmarklet tab ──
  bodies.set('bookmarklet', renderBookmarkletPanel((p) => api.loadPayload(p)));

  // ── Pack tab (Spider wizard) ──
  bodies.set('pack', mountPackWizard().el);
  window.addEventListener('krosspoint:pack-opened', () => api.selectTab('pack'));

  // ── Layout ──
  const tabList = h('div', { class: 'tabs', role: 'tablist', 'aria-label': 'Input source' });
  const panelHost = h('div', { class: 'tab-host' });
  for (const [id, label] of TABS) {
    const btn = h(
      'button',
      { type: 'button', role: 'tab', class: 'tab', id: `tab-${id}`, 'aria-selected': 'false', onclick: () => api.selectTab(id) },
      label,
    );
    tabButtons.set(id, btn);
    tabList.appendChild(btn);
    const body = bodies.get(id)!;
    body.setAttribute('role', 'tabpanel');
    body.setAttribute('aria-labelledby', `tab-${id}`);
    body.hidden = true;
    panelHost.appendChild(body);
  }
  const busy = h('div', { class: 'busy-line', 'aria-live': 'polite' });
  store().subscribe((s) => {
    busy.textContent = s.busy ? 'Working...' : '';
  }, ['busy']);

  replace(root, h('h2', null, 'Source'), tabList, panelHost, busy);

  const api: SourcePanel = {
    selectTab(tab) {
      active = tab;
      for (const [id, btn] of tabButtons) {
        const on = id === tab;
        btn.setAttribute('aria-selected', String(on));
        btn.classList.toggle('active', on);
        bodies.get(id)!.hidden = !on;
      }
      const focusEl = bodies.get(tab)!.querySelector<HTMLElement>('input, textarea');
      focusEl?.focus({ preventScroll: true });
    },
    convert() {
      const fn = convertFns.get(active);
      if (fn) fn();
      else if (active === 'pack') toast('Use the buttons of the pack wizard.', 'info');
      else toast('This tab has no convert action. Choose a file or load a payload.', 'info');
    },
    loadPayload(p) {
      void runExtract({ kind: 'bookmarklet', content: p.html, sourceUrl: p.url }).then((a) => {
        if (a) toast(`Captured: ${a.title}`, 'ok');
      });
    },
    loadFiles(files) {
      const all = Array.from(files);
      const file = all[0];
      if (!file) return;
      // A file that is not an article (firmware.bin, .epub, an image): do not
      // convert it. Offer the "Send a file" section in the Device panel.
      if (!isArticleFile(file)) {
        const others = all.filter((f) => !isArticleFile(f));
        offerSendFile(others);
        toast(
          `${file.name} is not an article. It is ready in Device > Send a file: press Send file to put it on the reader as it is.`,
          'info',
          6000,
        );
        return;
      }
      if (files.length > 1) toast('Only the first file is used.', 'info');
      fileToInput(file)
        .then((input) => runExtract(input))
        .then((a) => {
          if (a) toast(`Loaded ${file.name}`, 'ok');
        })
        .catch((err) => toast(`Could not read ${file.name}: ${errorMessage(err)}`, 'error'));
    },
    fetchUrl(url) {
      api.selectTab('url');
      urlInput.value = url;
      void doFetch(url, urlStatus, urlFail);
    },
  };

  const last = store().get().settings.lastInputKind;
  const start: SourceTab = last && last !== 'bookmarklet' && TABS.some(([t]) => t === last) ? (last as SourceTab) : 'url';
  api.selectTab(start);
  return api;
}
