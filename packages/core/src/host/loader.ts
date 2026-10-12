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

export function resolveLibrary(url: string): Library | null {
  let u = url.trim();
  if (u.startsWith('//')) u = 'https:' + u;
  u = u.split(/[?#]/)[0];
  for (const lib of LIBRARIES) if (lib.patterns.some((p) => p.test(u))) return lib;
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
