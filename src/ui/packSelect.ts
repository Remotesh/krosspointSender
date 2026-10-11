/**
 * Pack wizard step 3: select pages. Checkbox list with title, depth badge,
 * flags and link count. Search filter, select all / none, remove a row,
 * add a page by title or URL. The counter "N selected / max M" blocks
 * a selection over the limit.
 */
import type { Pack, PackCandidate, PackPage } from '../types';
import { normalizeWikipediaTitle, webKey } from '../spider';
import { normalizeUrl } from '../extractors';
import { h, replace } from './dom';
import { toast } from './toast';

export interface SelectStepHooks {
  /** Change the pack (the caller puts it in the store). */
  update(pack: Pack): void;
}

const FLAG_TEXT: Record<string, string> = { disambiguation: 'disambig', redirect: 'redirect', manual: 'added' };

export function selectedCount(pack: Pack): number {
  return pack.pages.filter((p) => p.candidate.selected).length;
}

/** Make a candidate from user input: a Wikipedia title or URL, or a web URL. */
export function manualCandidate(pack: Pack, input: string): PackCandidate | string {
  const v = input.trim();
  if (!v) return 'Type a title or a URL.';
  if (pack.kind === 'wikipedia') {
    const rootLang = /^https?:\/\/([a-z-]+)\.wikipedia\.org/i.exec(pack.rootUrl)?.[1] ?? 'en';
    try {
      const n = normalizeWikipediaTitle(v, rootLang);
      return { key: n.title, title: n.title, url: n.url, depth: 1, linkCount: 0, selected: true, flags: ['manual'] };
    } catch (err) {
      return err instanceof Error ? err.message : String(err);
    }
  }
  const url = normalizeUrl(v);
  if (!url) return 'Type a full http(s) URL.';
  const key = webKey(url) ?? url;
  return { key, title: key, url: key, depth: 1, linkCount: 0, selected: true, flags: ['manual'] };
}

export function renderSelectStep(getPack: () => Pack, hooks: SelectStepHooks): HTMLElement {
  let filter = '';
  const counter = h('div', { class: 'pack-counter', 'aria-live': 'polite' });
  const list = h('ul', { class: 'pack-cands' });
  const search = h('input', { type: 'search', placeholder: 'Filter by title', 'aria-label': 'Filter pages' });
  const addInput = h('input', { type: 'text', placeholder: 'Title or URL of a page to add' });

  search.addEventListener('input', () => {
    filter = search.value.trim().toLowerCase();
    render();
  });

  function setPages(pages: PackPage[]): void {
    hooks.update({ ...getPack(), pages });
    render();
  }

  function visible(pack: Pack): PackPage[] {
    return filter ? pack.pages.filter((p) => p.candidate.title.toLowerCase().includes(filter)) : pack.pages;
  }

  function toggle(key: string, on: boolean): boolean {
    const pack = getPack();
    if (on && selectedCount(pack) >= pack.limits.maxArticles) {
      toast(`The limit is ${pack.limits.maxArticles} pages. Clear a page first, or raise "Max articles" in step 2.`, 'warn');
      return false;
    }
    setPages(pack.pages.map((p) => (p.candidate.key === key ? { ...p, candidate: { ...p.candidate, selected: on } } : p)));
    return true;
  }

  function selectAll(): void {
    const pack = getPack();
    let room = pack.limits.maxArticles - selectedCount(pack);
    const show = new Set(visible(pack).map((p) => p.candidate.key));
    let skipped = 0;
    const pages = pack.pages.map((p) => {
      if (p.candidate.selected || !show.has(p.candidate.key)) return p;
      if (room <= 0) {
        skipped++;
        return p;
      }
      room--;
      return { ...p, candidate: { ...p.candidate, selected: true } };
    });
    setPages(pages);
    if (skipped) toast(`${skipped} page(s) not selected: the limit is ${pack.limits.maxArticles}.`, 'info');
  }

  function selectNone(): void {
    const pack = getPack();
    const show = new Set(visible(pack).map((p) => p.candidate.key));
    // The root stays selected. The pack needs it.
    setPages(
      pack.pages.map((p) =>
        show.has(p.candidate.key) && p.candidate.key !== pack.rootKey
          ? { ...p, candidate: { ...p.candidate, selected: false } }
          : p,
      ),
    );
  }

  function remove(key: string): void {
    const pack = getPack();
    if (key === pack.rootKey) {
      toast('The root page stays in the pack.', 'info');
      return;
    }
    setPages(pack.pages.filter((p) => p.candidate.key !== key));
  }

  function add(): void {
    const pack = getPack();
    const c = manualCandidate(pack, addInput.value);
    if (typeof c === 'string') {
      toast(c, 'warn');
      return;
    }
    const old = pack.pages.find((p) => p.candidate.key === c.key);
    if (old) {
      if (!old.candidate.selected) toggle(c.key, true);
      toast(`"${c.title}" is already in the list.`, 'info');
    } else {
      const sel = selectedCount(pack) < pack.limits.maxArticles;
      if (!sel) toast(`Added, but not selected: the limit is ${pack.limits.maxArticles}.`, 'warn');
      setPages([...pack.pages, { candidate: { ...c, selected: sel }, state: 'queued' }]);
    }
    addInput.value = '';
  }
  addInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') add();
  });

  function row(p: PackPage, pack: Pack): HTMLElement {
    const c = p.candidate;
    const box = h('input', { type: 'checkbox', checked: c.selected, 'aria-label': `Select ${c.title}` });
    box.addEventListener('change', () => {
      if (!toggle(c.key, box.checked)) box.checked = false;
    });
    const flags = (c.flags ?? []).map((f) => h('span', { class: `badge flag-${f}` }, FLAG_TEXT[f] ?? f));
    return h(
      'li',
      { class: `pack-cand${c.selected ? ' on' : ''}`, dataset: { key: c.key } },
      h('label', { class: 'pack-cand-main' }, box, ' ', h('span', { class: 'pack-cand-title' }, c.title)),
      h('span', { class: `badge depth-${c.depth}`, title: 'Depth' }, `d${c.depth}`),
      ...flags,
      c.linkCount ? h('span', { class: 'hint', title: 'Pages that link here' }, `${c.linkCount} link${c.linkCount === 1 ? '' : 's'}`) : null,
      c.key === pack.rootKey
        ? h('span', { class: 'badge' }, 'root')
        : h('button', { type: 'button', class: 'btn small', 'aria-label': `Remove ${c.title}`, onclick: () => remove(c.key) }, 'x'),
    );
  }

  function render(): void {
    const pack = getPack();
    const n = selectedCount(pack);
    counter.textContent = `${n} selected / max ${pack.limits.maxArticles} · ${pack.pages.length} found`;
    counter.classList.toggle('full', n >= pack.limits.maxArticles);
    const rows = visible(pack);
    replace(list, ...(rows.length ? rows.map((p) => row(p, pack)) : [h('li', { class: 'hint' }, 'No page matches the filter.')]));
  }

  const el = h(
    'div',
    { class: 'pack-step' },
    h('div', { class: 'row' }, search),
    h(
      'div',
      { class: 'btn-row' },
      h('button', { type: 'button', class: 'btn small', onclick: selectAll }, 'Select all'),
      h('button', { type: 'button', class: 'btn small', onclick: selectNone }, 'Select none'),
    ),
    counter,
    list,
    h('div', { class: 'row' }, addInput, h('button', { type: 'button', class: 'btn small', onclick: add }, 'Add page')),
  );
  render();
  return el;
}
