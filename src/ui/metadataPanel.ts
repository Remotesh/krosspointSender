/**
 * Metadata panel. The user edits title, author, date, source URL, language
 * and tags. Edits go back to the article in the store. The filename
 * preview updates live.
 */
import type { Article } from '../types';
import { makeFilename } from '../converters';
import { field, h, replace } from './dom';
import { persistSettings } from './actions';
import { store } from './state';

/** Return the date part (YYYY-MM-DD) of an ISO string. */
function dateOnly(iso?: string): string {
  if (!iso) return '';
  const m = /^\d{4}-\d{2}-\d{2}/.exec(iso);
  if (m) return m[0];
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? '' : d.toISOString().slice(0, 10);
}

export function mountMetadataPanel(root: HTMLElement): void {
  const st = store();
  const title = h('input', { type: 'text', required: true });
  const author = h('input', { type: 'text', placeholder: 'Unknown' });
  const date = h('input', { type: 'date' });
  const source = h('input', { type: 'url', placeholder: 'https://...' });
  const lang = h('input', { type: 'text', placeholder: 'en', size: 6 });
  const tags = h('input', { type: 'text', placeholder: 'tag one, tag two' });
  const template = h('input', { type: 'text', spellcheck: false });
  const preview = h('output', { class: 'filename-preview', 'aria-live': 'polite' });
  const inputs = [title, author, date, source, lang, tags];

  const bind = (el: HTMLInputElement, fn: (v: string) => Partial<Article>): void => {
    el.addEventListener('input', () => st.updateArticle(fn(el.value)));
  };
  bind(title, (v) => ({ title: v || 'Untitled' }));
  bind(author, (v) => ({ author: v.trim() || undefined }));
  bind(date, (v) => ({ publishedAt: v || undefined }));
  bind(source, (v) => ({ sourceUrl: v.trim() || undefined }));
  bind(lang, (v) => ({ language: v.trim() || undefined }));
  bind(tags, (v) => ({
    tags: v
      .split(',')
      .map((t) => t.trim())
      .filter(Boolean),
  }));

  let saveTimer = 0;
  template.addEventListener('input', () => {
    st.updateSettings((s) => ({ ...s, filename: { ...s.filename, template: template.value } }));
    window.clearTimeout(saveTimer);
    saveTimer = window.setTimeout(() => void persistSettings(), 600);
  });

  const body = h(
    'div',
    { class: 'meta-grid' },
    field('Title', title),
    field('Author', author),
    field('Published', date),
    field('Language', lang),
    field('Source URL', source),
    field('Tags (comma separated)', tags),
    field('Filename template', template, 'Fields: {title} {author} {date} {site}'),
    h('div', { class: 'field' }, h('span', { class: 'label' }, 'Filename'), preview),
  );
  const empty = h('p', { class: 'hint' }, 'Load an article to edit its metadata.');

  // ── Pack mode: title, author, filename of the pack EPUB ──
  const pTitle = h('input', { type: 'text', required: true });
  const pAuthor = h('input', { type: 'text' });
  const pFile = h('input', { type: 'text', spellcheck: false });
  pTitle.addEventListener('input', () => {
    const pack = st.get().pack;
    if (pack) st.set({ pack: { ...pack, title: pTitle.value || 'Untitled pack' } });
  });
  pAuthor.addEventListener('input', () => st.set({ packAuthor: pAuthor.value }));
  pFile.addEventListener('input', () => st.set({ packFileName: pFile.value.trim() || undefined }));
  const packBody = h(
    'div',
    { class: 'meta-grid', hidden: true },
    field('Pack title', pTitle),
    field('Author', pAuthor, 'Empty: "Wikipedia contributors" for a Wikipedia pack.'),
    field('Filename', pFile, 'The EPUB is built again when the title or the author changes.'),
  );
  replace(root, h('h2', null, 'Metadata'), empty, body, packBody);

  let shownId = '';
  function fill(a: Article): void {
    title.value = a.title;
    author.value = a.author ?? '';
    date.value = dateOnly(a.publishedAt);
    source.value = a.sourceUrl ?? '';
    lang.value = a.language ?? '';
    tags.value = a.tags.join(', ');
  }

  let packShownId = '';
  function renderPack(): boolean {
    const { mode, pack, packAuthor, packFileName, packOutput } = st.get();
    const on = mode === 'pack' && !!pack;
    packBody.hidden = !on;
    if (!on || !pack) {
      packShownId = '';
      return false;
    }
    body.hidden = true;
    empty.hidden = true;
    if (pack.id !== packShownId || document.activeElement !== pTitle) pTitle.value = pack.title;
    if (document.activeElement !== pAuthor) pAuthor.value = packAuthor ?? '';
    if (document.activeElement !== pFile) pFile.value = packFileName ?? packOutput?.file.name ?? '';
    packShownId = pack.id;
    return true;
  }

  function render(): void {
    if (renderPack()) return;
    body.hidden = false;
    const { article, settings, format } = st.get();
    empty.hidden = !!article;
    for (const el of inputs) el.disabled = !article;
    if (document.activeElement !== template) template.value = settings.filename.template;
    if (!article) {
      preview.textContent = '';
      shownId = '';
      return;
    }
    // Fill the inputs only for a new article. Then typing is not disturbed.
    if (article.id !== shownId) {
      shownId = article.id;
      fill(article);
    }
    try {
      preview.textContent = makeFilename(article, format, settings.filename);
    } catch {
      preview.textContent = '(filename error)';
    }
  }

  st.subscribe(render, ['article', 'settings', 'format', 'mode', 'pack', 'packAuthor', 'packFileName', 'packOutput']);
  render();
}
