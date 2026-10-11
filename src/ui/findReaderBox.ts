/**
 * "Find reader" control for a host field: the button, an optional subnet
 * input, a progress line, Cancel, and the list of found readers.
 * Clicking a reader calls onPick (the caller fills the host field).
 */
import { findReader, type FoundReader } from '../discovery/findReader';
import { errorMessage, h, replace } from './dom';

export interface FindReaderBox {
  /** The "Find reader" button. Put it next to the host input. */
  button: HTMLButtonElement;
  /** The panel with the subnet input, progress, Cancel and results. */
  panel: HTMLElement;
  /** Stops a running search. */
  cancel(): void;
}

export interface FindReaderBoxOptions {
  /** Hosts of the saved profiles. Their /24 is swept first. */
  profileHosts: () => string[];
  onPick: (reader: FoundReader) => void;
}

const SOURCE_TEXT: Record<FoundReader['source'], string> = {
  helper: 'desktop helper',
  mdns: 'crosspoint.local',
  sweep: 'subnet sweep',
};

export function readerLabel(r: FoundReader): string {
  const bits = [r.name, r.version ? `firmware ${r.version}` : undefined, r.ip ? r.ip : undefined, `via ${SOURCE_TEXT[r.source]}`];
  return `${r.host} (${bits.filter(Boolean).join(', ')})`;
}

export function createFindReaderBox(opts: FindReaderBoxOptions): FindReaderBox {
  let ctrl: AbortController | null = null;
  const subnet = h('input', {
    type: 'text',
    class: 'subnet-input',
    placeholder: '192.168.1',
    inputmode: 'decimal',
    autocomplete: 'off',
    'aria-label': 'Subnet to search (optional)',
  });
  const progress = h('p', { class: 'hint find-progress', role: 'status', 'aria-live': 'polite' });
  const list = h('ul', { class: 'find-results' });
  const notes = h('ul', { class: 'hint find-notes' });
  const cancelBtn = h('button', { type: 'button', class: 'btn small', hidden: true }, 'Cancel');
  const button = h('button', { type: 'button', class: 'btn small find-btn' }, 'Find reader');
  const panel = h(
    'div',
    { class: 'find-reader', hidden: true },
    h('label', { class: 'subnet-row' }, 'Subnet (optional) ', subnet, ' .x'),
    h('div', { class: 'btn-row' }, progress, cancelBtn),
    list,
    notes,
  );

  function renderList(found: FoundReader[]): void {
    replace(
      list,
      ...found.map((r) =>
        h(
          'li',
          null,
          h('button', { type: 'button', class: 'linkish', onclick: () => opts.onPick(r) }, readerLabel(r)),
        ),
      ),
    );
  }

  function cancel(): void {
    ctrl?.abort();
  }

  async function run(): Promise<void> {
    if (ctrl) return;
    ctrl = new AbortController();
    panel.hidden = false;
    button.disabled = true;
    cancelBtn.hidden = false;
    replace(list);
    replace(notes);
    try {
      const res = await findReader(
        { signal: ctrl.signal, subnet: subnet.value, profileHosts: opts.profileHosts() },
        (p) => {
          progress.textContent = p.message;
          renderList(p.found);
        },
      );
      renderList(res.readers);
      progress.textContent = res.cancelled
        ? `Cancelled. ${res.readers.length ? 'Choose a reader below.' : ''}`.trim()
        : res.readers.length
          ? 'Choose your reader to fill the host.'
          : 'No reader found. On the reader open File Transfer and check that it is on the same Wi-Fi, or type the IP address it shows.';
      // Show the notes when nothing was found, or when a stage was skipped for https.
      const shown = res.readers.length ? res.notes.filter((n) => n.includes('https')) : res.notes;
      replace(notes, ...shown.map((n) => h('li', null, n)));
    } catch (err) {
      progress.textContent = `The search failed: ${errorMessage(err)}`;
    } finally {
      ctrl = null;
      button.disabled = false;
      cancelBtn.hidden = true;
    }
  }

  button.addEventListener('click', () => void run());
  cancelBtn.addEventListener('click', cancel);
  return { button, panel, cancel };
}
