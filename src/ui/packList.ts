/**
 * Center panel in pack mode: the article list of the pack. Each row has a
 * state pill. A click on a fetched page shows its cleaned text in the
 * device frame. A switch goes back to the single article.
 */
import type { PackPage } from '../types';
import { h, replace } from './dom';
import { packStats } from './packOutput';
import { store } from './state';

export function mountPackList(): HTMLElement {
  const st = store();
  const head = h('div', { class: 'pack-list-head' });
  const list = h('ol', { class: 'pack-list', 'aria-label': 'Pack articles' });
  const el = h('div', { class: 'pack-panel', hidden: true }, head, list);
  const switcher = h('div', { class: 'pack-switch', hidden: true });

  function shown(p: PackPage): boolean {
    return p.candidate.selected || p.state === 'done' || p.state === 'failed';
  }

  function render(): void {
    const { mode, pack, article, packPreviewKey } = st.get();
    // Article mode with a pack in memory: offer a way back to the pack.
    switcher.hidden = !(mode === 'article' && pack);
    if (!switcher.hidden && pack) {
      replace(switcher, h('button', { type: 'button', class: 'btn small', onclick: () => st.set({ mode: 'pack' }) }, `Show pack: ${pack.title}`));
    }
    el.hidden = mode !== 'pack' || !pack;
    if (el.hidden || !pack) return;
    const pages = pack.pages.filter(shown);
    const s = packStats(pack);
    const current = pages.find((p) => p.candidate.key === packPreviewKey && p.article) ?? pages.find((p) => p.article);
    replace(
      head,
      h('strong', null, pack.title),
      h('span', { class: 'hint' }, ` ${s.articles} / ${pages.length} fetched${s.failed ? `, ${s.failed} failed` : ''}`),
      article
        ? h('button', { type: 'button', class: 'btn small', onclick: () => st.set({ mode: 'article' }) }, 'Show single article')
        : null,
    );
    replace(
      list,
      ...pages.map((p) => {
        const on = current === p;
        const words = p.article ? `${p.article.wordCount.toLocaleString()} w` : '';
        return h(
          'li',
          { class: `pack-row${on ? ' current' : ''}` },
          h(
            'button',
            {
              type: 'button',
              class: 'pack-row-btn',
              disabled: !p.article,
              'aria-current': on ? 'true' : false,
              title: p.error ?? p.candidate.url,
              onclick: () => st.set({ packPreviewKey: p.candidate.key }),
            },
            h('span', { class: `pill pill-pack-${p.state}` }, p.state),
            ' ',
            h('span', { class: 'pack-row-title' }, p.candidate.title),
            words ? h('small', { class: 'hint' }, ` ${words}`) : null,
          ),
        );
      }),
    );
  }

  st.subscribe(render, ['mode', 'pack', 'packPreviewKey', 'article']);
  render();
  return h('div', null, switcher, el);
}
