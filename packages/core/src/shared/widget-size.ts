// A widget's size as the server counts it (docs/tgw-format.md): mini-HTML is at most 32 768 bytes of HTML and
// 4096 bytes after gzip at level 6 (PHP gzencode's default); past either the server refuses the widget with
// INPUT_MEDIA_WIDGET_TOO_LARGE. 3800 B of gzip is the margin the conformance set keeps (scripts/conformance/build.ts):
// another zlib can differ by a few bytes: Node's and Chromium's zlib (the harness, the dev app) count up to a few dozen
// bytes more than GNU gzip -6 on real widgets, so these numbers err on the safe side. Shared by the harness and the dev app.

export const MINI_HTML = { gzip: 4096, raw: 32768, warnGzip: 3800 } as const;

/** Bytes after gzip at level 6, and of the HTML as UTF-8. */
export interface WidgetSize { gzip: number; raw: number }

const kb = (n: number) => (n / 1024).toFixed(1) + ' KB';

/** `size: 2134 B gzip / 9.8 KB raw (limit 4096 / 32768)`. */
export function sizeLine(s: WidgetSize): string {
  return `size: ${s.gzip} B gzip / ${kb(s.raw)} raw (limit ${MINI_HTML.gzip} / ${MINI_HTML.raw})`;
}

/** What to tell the author: nothing, a warning close to the gzip limit, or an error past a limit. */
export function sizeVerdict(s: WidgetSize): { level: 'ok' | 'warning' | 'error'; message: string | null } {
  const over: string[] = [];
  if (s.gzip > MINI_HTML.gzip) over.push(`${s.gzip} B after gzip is over the ${MINI_HTML.gzip} B limit by ${s.gzip - MINI_HTML.gzip} B`);
  if (s.raw > MINI_HTML.raw) over.push(`${s.raw} B of HTML is over the ${MINI_HTML.raw} B limit by ${s.raw - MINI_HTML.raw} B`);
  if (over.length) return { level: 'error', message: `error: too large, the server refuses it (INPUT_MEDIA_WIDGET_TOO_LARGE): ${over.join('; ')}. Shorten the code, drop data: images, reuse classes.` };
  if (s.gzip > MINI_HTML.warnGzip) return { level: 'warning', message: `warning: ${s.gzip} B after gzip is ${MINI_HTML.gzip - s.gzip} B under the ${MINI_HTML.gzip} B limit; another zlib can count a few bytes more: aim for ${MINI_HTML.warnGzip} B or less.` };
  return { level: 'ok', message: null };
}
