// Loader: library resolution for <script src>. Trusted code, no DOM dependencies
// (also imported by the dev server for reference mode).
//
// Widgets may reference a small set of well-known libraries by CDN URL. Those
// URLs are never fetched: they are mapped to copies bundled with the client.

export interface Library {
  name: string;
  /** File name under /libs/ on the app origin. */
  file: string;
  /** Path inside node_modules of the bundled copy. */
  source: string;
  /** URL patterns (jsDelivr, cdnjs, unpkg) that resolve to this library. */
  patterns: RegExp[];
}

const V = String.raw`(?:@[\w.\-^~]+)?`;

export const LIBRARIES: Library[] = [
  {
    name: 'chart.js',
    file: 'chart.umd.js',
    source: 'chart.js/dist/chart.umd.min.js',
    patterns: [
      new RegExp(String.raw`^https?://cdn\.jsdelivr\.net/npm/chart\.js${V}(?:/dist/chart(?:\.umd)?(?:\.min)?\.js)?/?$`, 'i'),
      new RegExp(String.raw`^https?://unpkg\.com/chart\.js${V}(?:/dist/chart(?:\.umd)?(?:\.min)?\.js)?/?$`, 'i'),
      new RegExp(String.raw`^https?://cdnjs\.cloudflare\.com/ajax/libs/Chart\.js/[\w.\-]+/chart(?:\.umd)?(?:\.min)?\.js$`, 'i'),
    ],
  },
  {
    name: 'd3',
    file: 'd3.min.js',
    source: 'd3/dist/d3.min.js',
    patterns: [
      new RegExp(String.raw`^https?://cdn\.jsdelivr\.net/npm/d3${V}(?:/dist/d3(?:\.min)?\.js)?/?$`, 'i'),
      new RegExp(String.raw`^https?://unpkg\.com/d3${V}(?:/dist/d3(?:\.min)?\.js)?/?$`, 'i'),
      new RegExp(String.raw`^https?://cdnjs\.cloudflare\.com/ajax/libs/d3/[\w.\-]+/d3(?:\.min)?\.js$`, 'i'),
      new RegExp(String.raw`^https?://d3js\.org/d3\.v\d+(?:\.min)?\.js$`, 'i'),
    ],
  },
];

export interface ProvidedLibrarySpec {
  name: string;
  file: string;
  patterns: string[];
}

export const MAX_LIBRARY_URL = 2048;
const provided: Library[] = [];
const PROBES = ['', 'https://example.invalid/x.js', 'app:', 'https://cdn.jsdelivr.net/npm/x.js'];

export function provideLibrary(spec: ProvidedLibrarySpec): boolean {
  if (!spec || typeof spec.name !== 'string' || typeof spec.file !== 'string' || !Array.isArray(spec.patterns) || !spec.patterns.length) return false;
  if (!/^[a-z0-9][a-z0-9._-]{0,63}$/.test(spec.name) || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,124}\.js$/.test(spec.file) || spec.file.includes('..')) return false;
  if (LIBRARIES.some((l) => l.name === spec.name || l.file === spec.file)) return false;
  if (provided.some((l) => l.file === spec.file && l.name !== spec.name)) return false;
  const patterns: RegExp[] = [];
  for (const p of spec.patterns) {
    if (typeof p !== 'string' || p.length > 1024) return false;
    let re: RegExp;
    try { re = new RegExp(`^(?:${p})$`, 'i'); } catch { return false; }
    if (PROBES.some((probe) => re.test(probe))) return false;
    patterns.push(re);
  }
  const i = provided.findIndex((l) => l.name === spec.name);
  if (i >= 0) provided.splice(i, 1);
  provided.push({ name: spec.name, file: spec.file, source: '', patterns });
  return true;
}

export function clearProvidedLibraries(): void {
  provided.length = 0;
}

export function resolveLibrary(url: string): Library | null {
  if (typeof url !== 'string' || url.length > MAX_LIBRARY_URL) return null;
  let u = url.trim();
  if (u.startsWith('//')) u = 'https:' + u;
  u = u.split(/[?#]/)[0];
  if (/[\u0000-\u001f]/.test(u)) return null;
  for (const lib of LIBRARIES) if (lib.patterns.some((p) => p.test(u))) return lib;
  for (const lib of provided) if (lib.patterns.some((p) => p.test(u))) return lib;
  return null;
}

/**
 * Reference mode only (dev tooling): rewrite known CDN script URLs to the local
 * copies so that the natively rendered file needs no network either.
 */
export function rewriteForReference(html: string): string {
  const out = html.replace(/(<script\b[^>]*?\bsrc\s*=\s*)(["'])([^"']+)\2/gi, (m, pre, q, url) => {
    const lib = resolveLibrary(url);
    return lib ? `${pre}${q}/libs/${lib.file}${q}` : m;
  });
  // A fragment is rendered inside our (standards-mode) document in the product;
  // give it a doctype so the native comparison does not fall into quirks mode.
  return /^\s*<!doctype/i.test(out) ? out : '<!DOCTYPE html>\n' + out;
}
