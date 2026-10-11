/**
 * The UI store. It holds the current input, article, settings, device
 * profiles, output format and transfer status. Panels subscribe to it.
 * A change calls every listener with the set of changed keys.
 */
import type {
  AppSettings,
  Article,
  DeviceProfile,
  OutputFile,
  OutputFormat,
  Pack,
  RawInput,
  TransferProgress,
} from '../types';

export interface UiState {
  input?: RawInput;
  article?: Article;
  settings: AppSettings;
  profiles: DeviceProfile[];
  selectedProfileId?: string;
  format: OutputFormat;
  transfer: TransferProgress;
  /** Preview options. */
  showImages: boolean;
  zoom: number;
  /** True while extraction or a fetch runs. */
  busy: boolean;
  /** 'article' = one article is the output. 'pack' = the Spider pack is the output. */
  mode: 'article' | 'pack';
  /** The current Spider pack (scan, selection or built). */
  pack?: Pack;
  /** The built pack EPUB and the data it was built from. */
  packOutput?: PackOutput;
  /** Pack author for the EPUB (dc:creator). Empty = builder default. */
  packAuthor?: string;
  /** Pack output filename with extension. Empty = builder default. */
  packFileName?: string;
  /** Key of the pack page that the preview shows. */
  packPreviewKey?: string;
}

export interface PackOutput {
  file: OutputFile;
  images: Map<string, Blob>;
  /** Pack title and author at build time. A change makes a rebuild. */
  builtTitle: string;
  builtAuthor: string;
}

/** The article that the preview shows: the current article, or the chosen pack page. */
export function viewArticle(s: UiState): Article | undefined {
  if (s.mode !== 'pack') return s.article;
  const pages = s.pack?.pages ?? [];
  const chosen = pages.find((p) => p.candidate.key === s.packPreviewKey && p.article);
  return (chosen ?? pages.find((p) => p.article))?.article;
}

/** True when the right panel has something to export or send. */
export function hasOutput(s: UiState): boolean {
  return s.mode === 'pack' ? !!s.packOutput : !!s.article;
}

export type StateKey = keyof UiState;
export type Listener = (state: UiState, changed: Set<StateKey>) => void;

export interface Store {
  get(): UiState;
  set(patch: Partial<UiState>): void;
  /** Change the article in place and notify. */
  updateArticle(patch: Partial<Article>): void;
  /** Change the settings and notify. The caller saves them. */
  updateSettings(fn: (s: AppSettings) => AppSettings): void;
  subscribe(fn: Listener, keys?: StateKey[]): () => void;
  selectedProfile(): DeviceProfile | undefined;
}

/** Make a store with a start state. */
export function createStore(initial: UiState): Store {
  let state = initial;
  const listeners = new Set<{ fn: Listener; keys?: StateKey[] }>();

  function emit(changed: Set<StateKey>): void {
    for (const l of [...listeners]) {
      if (!l.keys || l.keys.some((k) => changed.has(k))) {
        try {
          l.fn(state, changed);
        } catch (err) {
          console.error('UI listener failed', err);
        }
      }
    }
  }

  return {
    get: () => state,
    set(patch) {
      state = { ...state, ...patch };
      emit(new Set(Object.keys(patch) as StateKey[]));
    },
    updateArticle(patch) {
      if (!state.article) return;
      state = { ...state, article: { ...state.article, ...patch } };
      emit(new Set<StateKey>(['article']));
    },
    updateSettings(fn) {
      state = { ...state, settings: fn(structuredClone(state.settings)) };
      emit(new Set<StateKey>(['settings']));
    },
    subscribe(fn, keys) {
      const entry = { fn, keys };
      listeners.add(entry);
      return () => listeners.delete(entry);
    },
    selectedProfile() {
      return state.profiles.find((p) => p.id === state.selectedProfileId);
    },
  };
}

/** The one store for the page. main.ts sets it at boot. */
let current: Store | undefined;

export function setStore(s: Store): void {
  current = s;
}

export function store(): Store {
  if (!current) throw new Error('UI store is not ready');
  return current;
}
