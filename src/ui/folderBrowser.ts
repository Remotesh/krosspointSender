/**
 * Folder browser for the device wizard ("Choose Destination Folder").
 * crosspoint-http and webdav: list the sub-folders of the current path
 * (listRemoteFolder), go up, make a new folder (makeRemoteFolder), and
 * "Use this folder". Other methods: a plain text field.
 * A typed path is always possible. The send creates a missing folder.
 */
import type { DeviceProfile } from '../types';
import { canBrowseRemote, listRemoteFolder, makeRemoteFolder, normalizeFolder } from '../transports';
import { errorMessage, field, h, replace } from './dom';
import { asTransferError } from './actions';
import { renderExplain } from './devicePanel';
import { toast } from './toast';

/** Parent of a folder path. "/" stays "/". */
export function parentFolder(path: string): string {
  const segs = normalizeFolder(path).split('/').filter(Boolean);
  segs.pop();
  return segs.length ? `/${segs.join('/')}/` : '/';
}

export function renderFolderBrowser(profile: DeviceProfile, onUse: (path: string) => void | Promise<void>): HTMLElement {
  const typed = h('input', { type: 'text', value: profile.targetFolder, spellcheck: false });
  const useTyped = h('button', {
    type: 'button',
    class: 'btn',
    onclick: () => void onUse(normalizeFolder(typed.value || '/')),
  }, 'Use typed path');
  const manual = field('Folder path', typed, 'Example: /Books/K-rosspoint/. The app creates a missing folder when it sends.');

  if (!canBrowseRemote(profile.method)) {
    return h(
      'div',
      { class: 'folder-browser' },
      h('p', { class: 'hint' }, `The method ${profile.method} cannot list folders on the device. Type the folder.`),
      manual,
      h('div', { class: 'btn-row' }, useTyped),
    );
  }

  let path = normalizeFolder(profile.targetFolder);
  const cur = h('code', { class: 'folder-path' });
  const list = h('ul', { class: 'folder-list', 'aria-label': 'Sub-folders' });
  const status = h('div', { 'aria-live': 'polite' });
  const btnUp = h('button', { type: 'button', class: 'btn small' }, 'Up');
  const btnNew = h('button', { type: 'button', class: 'btn small' }, 'New folder');
  const btnUse = h('button', { type: 'button', class: 'btn primary' }, 'Use this folder');

  async function load(p: string, fallbackToRoot = false): Promise<void> {
    path = normalizeFolder(p);
    cur.textContent = path;
    btnUp.disabled = path === '/';
    replace(status, h('p', { class: 'hint' }, 'Reading the folder...'));
    replace(list);
    try {
      const entries = await listRemoteFolder(profile, path);
      const dirs = entries.filter((e) => e.isDirectory && !e.name.startsWith('.'));
      const files = entries.length - dirs.length;
      replace(status, h('p', { class: 'hint' }, `${dirs.length} folder(s), ${files} other item(s).`));
      replace(
        list,
        ...(dirs.length
          ? dirs.map((d) =>
              h('li', null, h('button', { type: 'button', class: 'linkish folder-item', onclick: () => void load(`${path}${d.name}/`) }, `${d.name}/`)),
            )
          : [h('li', { class: 'hint' }, 'No sub-folders.')]),
      );
    } catch (err) {
      // A missing folder: try the root once, so the user can browse from there.
      if (fallbackToRoot && path !== '/') {
        toast(`${path} cannot be read (${errorMessage(err)}). Showing the root.`, 'info');
        await load('/');
        return;
      }
      replace(status, renderExplain(asTransferError(err)));
    }
  }

  btnUp.addEventListener('click', () => void load(parentFolder(path)));
  btnNew.addEventListener('click', async () => {
    const name = prompt(`New folder in ${path}:`, 'K-rosspoint');
    if (!name) return;
    try {
      await makeRemoteFolder(profile, path, name);
      toast(`Folder ${path}${name.trim()}/ created.`, 'ok');
      await load(`${path}${name.trim()}/`);
    } catch (err) {
      toast(`Could not create the folder: ${errorMessage(err)}`, 'error');
    }
  });
  btnUse.addEventListener('click', () => void onUse(path));

  void load(path, true);
  return h(
    'div',
    { class: 'folder-browser' },
    h('div', { class: 'row folder-head' }, h('span', null, 'Current: '), cur),
    h('div', { class: 'btn-row' }, btnUp, btnNew, btnUse),
    status,
    list,
    h('details', null, h('summary', null, 'Or type a path'), manual, h('div', { class: 'btn-row' }, useTyped)),
  );
}
