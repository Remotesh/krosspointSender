/**
 * Spider pack wizard: the [ pack ] tab of the source panel.
 * Steps: 1 root, 2 scan, 3 select, 4 build, 5 done.
 * Step 1 takes a Wikipedia URL or title (with a language), or the current
 * article and its links (web pack). Step 2 sets depth and limits and runs
 * the scan with progress and cancel. Steps 3 to 5 are in packSelect.ts
 * and packBuild.ts.
 */
import type { Article, Pack, PackCandidate, PackLimits, PackSourceKind } from '../types';
import { newPack, normalizeWikipediaTitle, scanWebLinks, scanWikipediaLinks, webKey } from '../spider';
import { isWikipediaUrl } from '../extractors';
import { checkbox, errorMessage, field, h, replace, select } from './dom';
import { renderSelectStep } from './packSelect';
import { renderBuildStep, renderDoneStep } from './packBuild';
import { store } from './state';
import { toast } from './toast';

const STEPS = ['Root', 'Scan', 'Select', 'Build', 'Done'];

interface Root {
  kind: PackSourceKind;
  key: string;
  title: string;
  url: string;
  lang: string;
  /** Web pack: the loaded article. */
  article?: Article;
}

export interface PackWizard {
  el: HTMLElement;
  /** Show a step (0-based). */
  go(step: number): void;
}

export function mountPackWizard(): PackWizard {
  const st = store();
  const el = h('div', { class: 'tab-body pack-wizard' });
  let step = 0;
  let root: Root | undefined;
  let limits: PackLimits = { ...st.get().settings.packLimits };
  let source: 'wiki' | 'web' = 'wiki';
  const wikiInput = h('input', { type: 'text', placeholder: 'https://en.wikipedia.org/wiki/Steam_engine or Steam engine' });
  const wikiLang = h('input', { type: 'text', value: 'en', size: 4, maxlength: 12, 'aria-label': 'Language code' });
  let ctrl: AbortController | undefined;

  function updatePack(pack: Pack): void {
    // A changed selection makes the built EPUB old.
    st.set({ pack, packOutput: undefined, mode: 'pack' });
  }

  // ── Step 1: root ──
  function stepRoot(): HTMLElement {
    const art = st.get().article;
    const wikiRadio = h('input', { type: 'radio', name: 'pack-src', value: 'wiki', checked: source === 'wiki' });
    const webRadio = h('input', { type: 'radio', name: 'pack-src', value: 'web', checked: source === 'web', disabled: !art });
    wikiRadio.addEventListener('change', () => (source = 'wiki'));
    webRadio.addEventListener('change', () => (source = 'web'));
    if (!wikiInput.value && art?.sourceUrl && isWikipediaUrl(art.sourceUrl)) wikiInput.value = art.sourceUrl;
    const next = (): void => {
      try {
        if (source === 'web') {
          const a = st.get().article;
          if (!a) throw new Error('Load an article first.');
          const base = a.sourceUrl ?? '';
          const key = (base && webKey(base)) || base || a.id;
          root = { kind: 'web', key, title: a.title, url: base, lang: a.language ?? 'en', article: a };
        } else {
          const n = normalizeWikipediaTitle(wikiInput.value, wikiLang.value.trim() || 'en');
          root = { kind: 'wikipedia', key: n.title, title: n.title, url: n.url, lang: n.lang };
        }
        go(1);
      } catch (err) {
        toast(errorMessage(err), 'warn');
      }
    };
    wikiInput.onkeydown = (e) => {
      if (e.key === 'Enter') next();
    };
    return h(
      'div',
      { class: 'pack-step' },
      h('p', { class: 'hint' }, 'A pack is one EPUB with a root article and the articles it links to. Links between them work on the reader.'),
      h('label', { class: 'check' }, wikiRadio, ' Wikipedia article'),
      field('Wikipedia URL or title', wikiInput),
      field('Language code', wikiLang, 'Used when you type a title. A URL sets its own language.'),
      h('label', { class: `check${art ? '' : ' muted'}` }, webRadio, art ? ` Current article + its links: ${art.title}` : ' Current article + its links (load an article first)'),
      h('div', { class: 'btn-row' }, h('button', { type: 'button', class: 'btn primary', onclick: next }, 'Next')),
    );
  }

  // ── Step 2: scan ──
  function num(label: string, key: 'maxArticles' | 'maxImagesPerArticle' | 'maxTotalEpubSizeMB' | 'requestDelayMs', min: number, max: number): HTMLElement {
    const input = h('input', { type: 'number', min, max, value: String(limits[key]) });
    input.addEventListener('input', () => {
      const v = Number(input.value);
      if (input.value !== '' && Number.isFinite(v) && v >= min && v <= max) limits = { ...limits, [key]: v };
    });
    return field(label, input);
  }

  function stepScan(): HTMLElement {
    if (!root) return h('p', null, 'Choose a root first.');
    const r = root;
    const warn = h('p', { class: 'warn-text', hidden: limits.depth !== 2 }, 'Depth 2 reads many pages and can take minutes. Keep "Max articles" small.');
    const depth = select<'0' | '1' | '2'>(
      [['0', '0 - root only'], ['1', '1 - root + its links'], ['2', '2 - links of links']],
      String(limits.depth) as '0' | '1' | '2',
      (v) => {
        limits = { ...limits, depth: Number(v) as 0 | 1 | 2 };
        warn.hidden = limits.depth !== 2;
      },
    );
    const line = h('div', { class: 'fetch-status', 'aria-live': 'polite' });
    const btnScan = h('button', { type: 'button', class: 'btn primary' }, 'Scan');
    const btnCancel = h('button', { type: 'button', class: 'btn', disabled: true }, 'Cancel');

    async function scan(): Promise<void> {
      ctrl = new AbortController();
      btnScan.disabled = true;
      btnCancel.disabled = false;
      line.className = 'fetch-status busy';
      line.textContent = 'Scanning...';
      try {
        let cands: PackCandidate[];
        if (r.kind === 'wikipedia') {
          cands = await scanWikipediaLinks(
            { lang: r.lang, title: r.title },
            limits,
            (p) => {
              line.textContent = `scanned ${p.done} / ${p.total} pages${p.currentTitle ? ` - ${p.currentTitle}` : ''}${p.message ? ` (${p.message})` : ''}`;
            },
            ctrl.signal,
          );
        } else {
          const rootCand: PackCandidate = { key: r.key, title: r.title, url: r.url, depth: 0, linkCount: 0, selected: true };
          const found = limits.depth === 0 ? [] : scanWebLinks(r.article!, r.url, limits).filter((c) => c.key !== r.key);
          cands = [rootCand, ...found];
        }
        const rootCand = cands.find((c) => c.depth === 0) ?? cands[0];
        const pack = newPack(r.kind, rootCand?.key ?? r.key, rootCand?.url ?? r.url, `${rootCand?.title ?? r.title} pack`, limits);
        pack.pages = cands.map((c) => ({ candidate: c, state: 'queued' as const }));
        // Web pack: the root is already loaded. fetchPackPages does not fetch a done page again.
        if (r.kind === 'web' && r.article) {
          pack.pages[0] = { ...pack.pages[0], state: 'done', article: r.article };
        }
        st.set({ pack, packOutput: undefined, mode: 'pack', packPreviewKey: undefined, packFileName: undefined });
        line.className = 'fetch-status ok';
        line.textContent = `found ${cands.length} pages (${cands.filter((c) => c.selected).length} selected).`;
        go(2);
      } catch (err) {
        line.className = 'fetch-status bad';
        line.textContent = (err as { name?: string }).name === 'AbortError' ? 'Scan cancelled.' : `Scan failed: ${errorMessage(err)}`;
      } finally {
        ctrl = undefined;
        btnScan.disabled = false;
        btnCancel.disabled = true;
      }
    }
    btnScan.addEventListener('click', () => void scan());
    btnCancel.addEventListener('click', () => ctrl?.abort());

    return h(
      'div',
      { class: 'pack-step' },
      h('p', null, h('strong', null, r.title), ' ', h('small', { class: 'hint' }, r.url)),
      field('Depth', depth),
      warn,
      h(
        'details',
        { class: 'pack-limits', open: true },
        h('summary', null, 'Limits'),
        num('Max articles', 'maxArticles', 1, 500),
        num('Max images per article', 'maxImagesPerArticle', 0, 50),
        num('Max EPUB size (MB)', 'maxTotalEpubSizeMB', 1, 500),
        num('Delay between requests (ms)', 'requestDelayMs', 0, 10000),
        checkbox('Include references', limits.includeReferences, (v) => (limits = { ...limits, includeReferences: v })),
        checkbox('Include infobox', limits.includeInfobox, (v) => (limits = { ...limits, includeInfobox: v })),
        r.kind === 'web' ? checkbox('Same site only', limits.sameDomainOnly, (v) => (limits = { ...limits, sameDomainOnly: v })) : null,
      ),
      h('div', { class: 'btn-row' }, btnScan, btnCancel),
      line,
    );
  }

  function stepSelect(): HTMLElement {
    const pack = st.get().pack;
    if (!pack) return h('p', null, 'Scan first.');
    return h(
      'div',
      null,
      renderSelectStep(() => st.get().pack!, { update: updatePack }),
      h('div', { class: 'btn-row' }, h('button', { type: 'button', class: 'btn primary', onclick: () => go(3) }, 'Next: build')),
    );
  }

  function go(n: number): void {
    ctrl?.abort();
    step = Math.max(0, Math.min(STEPS.length - 1, n));
    const pack = st.get().pack;
    const reachable = (i: number): boolean =>
      i === 0 || (i === 1 && !!root) || (i >= 2 && i <= 3 && !!pack) || (i === 4 && !!st.get().packOutput);
    const nav = h(
      'ol',
      { class: 'wizard-steps pack-steps' },
      ...STEPS.map((s, i) =>
        h(
          'li',
          { class: i === step ? 'current' : i < step ? 'done' : '', 'aria-current': i === step ? 'step' : false },
          reachable(i) && i !== step ? h('button', { type: 'button', class: 'linkish', onclick: () => go(i) }, s) : s,
        ),
      ),
    );
    const body =
      step === 0
        ? stepRoot()
        : step === 1
          ? stepScan()
          : step === 2
            ? stepSelect()
            : step === 3
              ? renderBuildStep({ done: () => go(4) })
              : renderDoneStep(
                  () => {
                    root = undefined;
                    limits = { ...st.get().settings.packLimits };
                    go(0);
                  },
                  () => go(2),
                );
    replace(el, h('h3', null, `Pack step ${step + 1}: ${STEPS[step]}`), nav, body);
  }

  // New pack limit defaults in Settings apply before a scan starts.
  let lastDefaults = JSON.stringify(st.get().settings.packLimits);
  st.subscribe((s) => {
    const now = JSON.stringify(s.settings.packLimits);
    if (now === lastDefaults) return;
    lastDefaults = now;
    if (step <= 1 && !ctrl) {
      limits = { ...s.settings.packLimits };
      if (step === 1) go(1);
    }
  }, ['settings']);

  // The library opened a stored pack: show step 5.
  window.addEventListener('krosspoint:pack-opened', () => go(4));

  go(0);
  return { el, go };
}
