// URL policy (security-critical, invariant 4).
//
// The ONLY URLs that may reach the renderer's DOM or CSS are:
//   - data: URLs with an allowlisted image MIME type
//   - blob: URLs minted by the host (none are minted in this prototype)
//   - same-document fragments ("#id")
// Everything else is rejected by returning null. There is deliberately no
// font or media kind: widgets use system fonts only (`@font-face` is dropped
// by the CSS sanitizer) and `<audio>`/`<video>` are dropped by the allowlist,
// so no font or codec parser is ever fed widget bytes.

import { PROFILE_IMAGES } from '../shared/profile-tables';

export type UrlKind = 'image' | 'fragment-only';

/** The profile's image types (PNG, JPEG, GIF, WebP: the decoder runs in the app process; SVG, AVIF, BMP and ICO are out). */
export const IMAGE_MIME = new Set<string>(PROFILE_IMAGES);

/** blob: URLs created by the host itself. Nothing else may use the blob: scheme. */
export const hostBlobUrls = new Set<string>();

/**
 * Strip the characters the URL parser ignores (ASCII tab/newline anywhere,
 * C0 control + space at the ends) so that "ja\nvascript:" style tricks and
 * leading whitespace cannot disguise a scheme.
 */
function normalize(raw: string): string {
  return raw.replace(/[\t\n\r]/g, '').replace(/^[\u0000- ]+|[\u0000- ]+$/g, '');
}

function dataMime(url: string): string | null {
  // data:[<mime>][;params][;base64],<data>
  const m = /^data:([^,;]*)(?:;[^,]*)?,/i.exec(url);
  return m ? m[1].trim().toLowerCase() : null;
}

/**
 * Returns the URL to use (normalized) if it is allowed for `kind`, else null.
 * Fragments are returned as-is. No relative URLs: there is nothing to resolve against.
 */
export function checkUrl(raw: string, kind: UrlKind): string | null {
  const url = normalize(String(raw));
  if (url.startsWith('#')) {
    // Fragment: must not contain anything that could re-open a URL context.
    return /^#[^\s"'()\\<>]*$/.test(url) ? url : null;
  }
  if (kind === 'fragment-only') return null;
  if (/^data:/i.test(url)) {
    const mime = dataMime(url);
    if (mime === null) return null;
    if (kind === 'image' && IMAGE_MIME.has(mime)) return url;
    return null;
  }
  if (/^blob:/i.test(url)) return hostBlobUrls.has(url) ? url : null;
  return null;
}

export function isFragment(raw: string): boolean {
  return normalize(String(raw)).startsWith('#');
}

/** 1×1 transparent placeholder used where an external image was stripped. */
export const PLACEHOLDER_IMAGE =
  'data:image/svg+xml,' +
  encodeURIComponent(
    '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48" viewBox="0 0 48 48">' +
      '<rect width="48" height="48" fill="#8882"/><path d="M8 36l10-12 8 9 6-7 8 10z" fill="#8886"/>' +
      '<circle cx="33" cy="15" r="4" fill="#8886"/></svg>',
  );
