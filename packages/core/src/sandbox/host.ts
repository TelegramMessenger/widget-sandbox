// The sandbox's only link to the outside world: the `__host` function installed
// by the worker glue. Everything in src/sandbox runs INSIDE QuickJS and is
// untrusted from the host's point of view — nothing here is a security boundary.

declare const __host: (kind: string, arg?: string) => any;
declare const __hostBinary: () => ArrayBuffer | null;

const hostFn = __host;
const hostBin = __hostBinary;

export const host = (kind: string, arg?: string): any => hostFn(kind, arg);
export const hostBinary = (): ArrayBuffer | null => hostBin();

export function diag(kind: string, detail: string): void {
  hostFn('diag', kind + ':' + detail);
}

const seenUnsupported = new Set<string>();
/** Record (once per API) that the widget touched something the runtime does not provide. */
export function unsupported(api: string): void {
  if (seenUnsupported.has(api)) return;
  seenUnsupported.add(api);
  diag('unsupported', api);
}

// ── sync queries with a layout cache ──

const cache = new Map<string, any>();
export const counters = { cacheHits: 0, missed: 0 };
let flushHook: () => void = () => {};
export function setFlushHook(fn: () => void) {
  flushHook = fn;
}

/** Layout may have changed: drop cached answers. Called after every flush that sent ops. */
export function invalidateLayout(): void {
  cache.clear();
}

/**
 * Synchronous round trip to the renderer. Pending mutations are flushed first so
 * the answer reflects the current DOM; answers are cached until the next flush.
 */
export function query(q: Record<string, unknown>, cacheable = true): any {
  flushHook();
  const key = cacheable ? JSON.stringify(q) : '';
  if (cacheable) {
    const hit = cache.get(key);
    if (hit !== undefined) {
      counters.cacheHits++;
      return hit;
    }
  }
  let result: any;
  try {
    result = JSON.parse(hostFn('query', JSON.stringify(q)));
  } catch {
    result = { error: 'bad result' };
  }
  if (cacheable) cache.set(key, result);
  return result;
}
