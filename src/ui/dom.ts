/**
 * Small DOM helpers. No framework.
 * h() makes an element with attributes, event handlers and children.
 */

export type Child = Node | string | number | null | undefined | false | Child[];

export type Attrs = Record<string, unknown> & {
  class?: string;
  style?: string;
};

/** Make an element. Keys that start with "on" become event listeners. */
export function h<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Attrs | null = null,
  ...children: Child[]
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag);
  // Children go first. Then a select can take its value from attrs.
  append(el, children);
  if (attrs) {
    for (const [key, value] of Object.entries(attrs)) {
      if (value === undefined || value === null || value === false) continue;
      if (key.startsWith('on') && typeof value === 'function') {
        el.addEventListener(key.slice(2).toLowerCase(), value as EventListener);
      } else if (key === 'class') {
        el.className = String(value);
      } else if (key === 'dataset' && typeof value === 'object') {
        Object.assign(el.dataset, value);
      } else if (key === 'value' || (key in el && typeof value !== 'string')) {
        // Properties such as checked, value (number), disabled.
        (el as unknown as Record<string, unknown>)[key] = value;
      } else if (value === true) {
        el.setAttribute(key, '');
      } else {
        el.setAttribute(key, String(value));
      }
    }
  }
  return el;
}

/** Append children. Flattens arrays and skips empty values. */
export function append(parent: Node, children: Child[]): void {
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue;
    if (Array.isArray(c)) append(parent, c);
    else if (c instanceof Node) parent.appendChild(c);
    else parent.appendChild(document.createTextNode(String(c)));
  }
}

/** Remove all children, then append new ones. */
export function replace(parent: Element, ...children: Child[]): void {
  parent.replaceChildren();
  append(parent, children);
}

let idCounter = 0;
/** Return a unique element id with a prefix. */
export function uid(prefix = 'k'): string {
  idCounter += 1;
  return `${prefix}-${idCounter}`;
}

/** A label + control pair. The label points to the control. */
export function field(label: string, control: HTMLElement, hint?: string): HTMLElement {
  if (!control.id) control.id = uid('f');
  return h(
    'div',
    { class: 'field' },
    h('label', { for: control.id }, label),
    control,
    hint ? h('small', { class: 'hint' }, hint) : null,
  );
}

/** A checkbox with its label on the right. */
export function checkbox(
  label: string,
  checked: boolean,
  onChange: (v: boolean) => void,
): HTMLLabelElement {
  const input = h('input', { type: 'checkbox', checked });
  input.addEventListener('change', () => onChange(input.checked));
  return h('label', { class: 'check' }, input, ' ', label);
}

/** A select element with string options. */
export function select<T extends string>(
  options: Array<[T, string]>,
  value: T,
  onChange: (v: T) => void,
): HTMLSelectElement {
  const el = h('select', null, ...options.map(([v, text]) => h('option', { value: v }, text)));
  el.value = value;
  el.addEventListener('change', () => onChange(el.value as T));
  return el;
}

/** Format a byte count for people. */
export function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / 1024 / 1024).toFixed(2)} MiB`;
}

/** Format an ISO date as a short local date. */
export function formatDate(iso?: string): string {
  if (!iso) return '';
  const d = new Date(iso);
  return Number.isNaN(d.getTime()) ? iso : d.toLocaleString();
}

/** Return a message for any thrown value. */
export function errorMessage(err: unknown): string {
  if (err instanceof Error) return err.message || err.name;
  return String(err);
}

export interface Drawer {
  el: HTMLElement;
  body: HTMLElement;
  open(): void;
  close(): void;
  isOpen(): boolean;
}

/**
 * Make a side drawer with a title and a close button.
 * Escape closes it. Focus goes back to the opener.
 */
export function createDrawer(title: string, onOpen?: () => void): Drawer {
  const titleId = uid('drawer-title');
  const body = h('div', { class: 'drawer-body' });
  const closeBtn = h('button', { type: 'button', class: 'btn small' }, 'Close');
  const panel = h(
    'aside',
    { class: 'drawer', role: 'dialog', 'aria-modal': 'true', 'aria-labelledby': titleId, tabindex: -1 },
    h('header', { class: 'drawer-head' }, h('h2', { id: titleId }, title), closeBtn),
    body,
  );
  const scrim = h('div', { class: 'scrim' });
  const el = h('div', { class: 'drawer-wrap', hidden: true }, scrim, panel);
  let opener: Element | null = null;

  const api: Drawer = {
    el,
    body,
    open() {
      opener = document.activeElement;
      el.hidden = false;
      onOpen?.();
      panel.focus();
    },
    close() {
      el.hidden = true;
      if (opener instanceof HTMLElement) opener.focus();
    },
    isOpen: () => !el.hidden,
  };
  closeBtn.addEventListener('click', () => api.close());
  scrim.addEventListener('click', () => api.close());
  el.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      e.stopPropagation();
      api.close();
    }
  });
  document.body.appendChild(el);
  return api;
}
