/**
 * Toast messages. They show at the bottom right and go away after a time.
 * The region is aria-live, so screen readers read each toast.
 */
import { h } from './dom';

export type ToastKind = 'info' | 'ok' | 'warn' | 'error';

let region: HTMLElement | undefined;

function ensureRegion(): HTMLElement {
  if (region && document.body.contains(region)) return region;
  region = h('div', { class: 'toasts', role: 'status', 'aria-live': 'polite' });
  document.body.appendChild(region);
  return region;
}

/** Show a toast. Errors stay longer. */
export function toast(message: string, kind: ToastKind = 'info', ms?: number): void {
  const r = ensureRegion();
  const close = h('button', { class: 'toast-close', type: 'button', 'aria-label': 'Close message' }, 'x');
  const el = h('div', { class: `toast toast-${kind}` }, h('span', null, message), close);
  const remove = (): void => el.remove();
  close.addEventListener('click', remove);
  r.appendChild(el);
  window.setTimeout(remove, ms ?? (kind === 'error' ? 9000 : 4000));
}
