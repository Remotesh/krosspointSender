/**
 * Abort and pacing helpers for the spider.
 */

/** Make the error that every spider function throws when the caller aborts. */
export function abortError(): DOMException {
  return new DOMException('The operation was cancelled.', 'AbortError');
}

/** Throw an AbortError when the signal is aborted. */
export function throwIfAborted(signal?: AbortSignal): void {
  if (signal?.aborted) throw abortError();
}

/** True when the error is an abort error. */
export function isAbortError(err: unknown): boolean {
  return (err as { name?: string } | null)?.name === 'AbortError';
}

/**
 * Wait `ms` milliseconds.
 * The promise rejects with an AbortError as soon as the signal aborts.
 */
export function sleep(ms: number, signal?: AbortSignal): Promise<void> {
  return new Promise<void>((resolve, reject) => {
    if (signal?.aborted) {
      reject(abortError());
      return;
    }
    const onAbort = () => {
      clearTimeout(timer);
      reject(abortError());
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', onAbort);
      resolve();
    }, Math.max(0, ms));
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

/**
 * Pacer: it keeps a minimum delay between requests.
 * Call `wait()` before each request. The first call does not wait.
 */
export class Pacer {
  private first = true;
  constructor(
    private readonly delayMs: number,
    private readonly signal?: AbortSignal,
  ) {}

  async wait(): Promise<void> {
    throwIfAborted(this.signal);
    if (this.first) {
      this.first = false;
      return;
    }
    if (this.delayMs > 0) await sleep(this.delayMs, this.signal);
  }
}
