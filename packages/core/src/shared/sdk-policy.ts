// SDK v1 checks (docs/sdk-v1.md §2.5, §2.6, §2.8, §2.9, §3). Pure and side-effect
// free, shared by the shim (which throws `TypeError` on what it rejects) and every
// TS glue (worker, WidgetHost, the Node harness), which check the same calls
// again because the shim is untrusted. Twins: SdkPolicy.swift, SdkPolicy.kt,
// sdk_policy.cpp; all four are checked against tests/fixtures/sdk-policy.json,
// so a rule change goes to the four copies and the fixture together.

export const MAX_LINK_LENGTH = 256;
/** Bytes of keys and values (UTF-8) per storage area and message. */
export const STORAGE_QUOTA_BYTES = 16384;
export const MAX_LOCALE_LENGTH = 35;
export const DEFAULT_LOCALE = 'en-US';
export const HAPTIC_BURST = 5;
export const HAPTIC_PER_SECOND = 10;
/** `haptic-limit` is reported at most this often. */
export const HAPTIC_REPORT_MS = 1000;
/** Handlers per `onEvent` type (and for the legacy `onTheme`); more are ignored. */
export const MAX_HANDLERS_PER_EVENT = 16;

export const THEME_KEYS = [
  'bg_color', 'text_color', 'hint_color', 'link_color', 'button_color', 'button_text_color',
  'secondary_bg_color', 'accent_text_color', 'subtitle_text_color', 'destructive_text_color',
  'section_bg_color', 'section_header_text_color', 'section_separator_color', 'header_bg_color', 'bottom_bar_bg_color',
] as const;
export type ThemeKey = (typeof THEME_KEYS)[number];
export type ThemeParams = Record<ThemeKey, string>;

export const HAPTIC_IMPACT_STYLES = ['light', 'medium', 'heavy', 'rigid', 'soft'];
export const HAPTIC_NOTIFICATION_TYPES = ['error', 'success', 'warning'];
export const TELEGRAM_HOSTS = ['t.me', 'telegram.me', 'telegram.dog'];

export type LinkMethod = 'openLink' | 'openTelegramLink';
export type HapticCall = { haptic: 'impact' | 'notification' | 'selection'; value?: string };
export type WidgetAction =
  | { kind: 'ready' }
  | { kind: 'openLink'; url: string }
  | { kind: 'openTelegramLink'; url: string }
  | ({ kind: 'haptic' } & HapticCall);

/** Methods that need the task's user gesture (docs/protocol.md, "User activation"). */
export const PRIVILEGED_METHODS = new Set(['openLink', 'openTelegramLink']);

/** UTF-8 length of a JS string; a lone surrogate counts 3 bytes (what U+FFFD takes). */
export function utf8Length(s: string): number {
  let n = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x80) n += 1;
    else if (c < 0x800) n += 2;
    else if (c >= 0xd800 && c <= 0xdbff && i + 1 < s.length && (s.charCodeAt(i + 1) & 0xfc00) === 0xdc00) { n += 4; i++; }
    else n += 3;
  }
  return n;
}

const HEX = '0123456789ABCDEF';
const isHex = (c: number) => (c >= 48 && c <= 57) || (c >= 65 && c <= 70) || (c >= 97 && c <= 102);
/** ASCII that may stand unescaped after the authority: RFC 3986 unreserved, reserved and `%`. */
const URL_ASCII = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~:/?#[]@!$&'()*+,;=%";

/** Lowercases A–Z only: a Unicode mapping (U+212A KELVIN SIGN → `k`) must not turn a non-ASCII host into an ASCII one. */
const asciiLower = (s: string) => s.replace(/[A-Z]/g, (c) => String.fromCharCode(c.charCodeAt(0) + 32));
const isTelegramHost = (host: string) => TELEGRAM_HOSTS.some((t) => host === t || host.endsWith('.' + t));

/**
 * The URL a link call opens, or null when it breaks the rules: an absolute
 * `https:` URL, no user name or password, an ASCII host of LDH labels (an IDN in
 * its `xn--` form), an optional port, at most 256 characters once non-ASCII
 * characters after the host are percent-encoded as UTF-8. `openTelegramLink`
 * takes only t.me, telegram.me and telegram.dog (and their subdomains), any
 * path; `openLink` takes every other host. Scheme and host come out lowercase,
 * the rest as given.
 */
export function checkLink(method: LinkMethod, url: unknown): string | null {
  if (typeof url !== 'string' || utf8Length(url) > MAX_LINK_LENGTH) return null;
  if (asciiLower(url.slice(0, 8)) !== 'https://') return null;
  let end = 8;
  while (end < url.length && url[end] !== '/' && url[end] !== '?' && url[end] !== '#') end++;
  const authority = url.slice(8, end);
  if (authority.includes('@')) return null;
  const colon = authority.indexOf(':');
  const host = asciiLower(colon < 0 ? authority : authority.slice(0, colon));
  const port = colon < 0 ? null : authority.slice(colon + 1);
  if (port !== null && !/^[0-9]{1,5}$/.test(port)) return null;
  if (port !== null && Number(port) > 65535) return null;
  if (host.length === 0 || host.length > 253) return null;
  for (const label of host.split('.')) {
    if (label.length === 0 || label.length > 63 || !/^[a-z0-9-]+$/.test(label) || label[0] === '-' || label[label.length - 1] === '-') return null;
  }
  if (isTelegramHost(host) !== (method === 'openTelegramLink')) return null;
  let rest = '';
  for (let i = end; i < url.length; i++) {
    const c = url.charCodeAt(i);
    if (c < 0x80) {
      if (!URL_ASCII.includes(url[i])) return null;
      if (c === 37 && !(i + 2 < url.length && isHex(url.charCodeAt(i + 1)) && isHex(url.charCodeAt(i + 2)))) return null;
      rest += url[i];
      continue;
    }
    let cp = c;
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < url.length && (url.charCodeAt(i + 1) & 0xfc00) === 0xdc00) cp = 0x10000 + ((c - 0xd800) << 10) + (url.charCodeAt(++i) - 0xdc00);
    else if (c >= 0xd800 && c <= 0xdfff) return null;
    const bytes = cp < 0x800 ? [0xc0 | (cp >> 6), 0x80 | (cp & 63)]
      : cp < 0x10000 ? [0xe0 | (cp >> 12), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63)]
      : [0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63), 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63)];
    for (const b of bytes) rest += '%' + HEX[b >> 4] + HEX[b & 15];
  }
  const out = 'https://' + host + (port === null ? '' : ':' + port) + rest;
  return out.length > MAX_LINK_LENGTH ? null : out;
}

/** The haptic of a `haptic` bridge call's args, or null: `['impact', style]`, `['notification', type]`, `['selection']`. */
export function checkHaptic(args: unknown): HapticCall | null {
  if (!Array.isArray(args)) return null;
  if (args[0] === 'impact' && args.length === 2 && HAPTIC_IMPACT_STYLES.includes(args[1])) return { haptic: 'impact', value: args[1] };
  if (args[0] === 'notification' && args.length === 2 && HAPTIC_NOTIFICATION_TYPES.includes(args[1])) return { haptic: 'notification', value: args[1] };
  if (args[0] === 'selection' && args.length === 1) return { haptic: 'selection' };
  return null;
}

/** The action of a bridge call, or null when the method is unknown or its arguments are wrong (`bridge-rejected`). */
export function checkBridgeCall(method: unknown, args: unknown): WidgetAction | null {
  if (!Array.isArray(args)) return null;
  switch (method) {
    case 'ready': return args.length === 0 ? { kind: 'ready' } : null;
    case 'openLink':
    case 'openTelegramLink': {
      const url = args.length === 1 ? checkLink(method, args[0]) : null;
      return url === null ? null : { kind: method, url };
    }
    case 'haptic': {
      const h = checkHaptic(args);
      return h === null ? null : { kind: 'haptic', ...h };
    }
    default: return null;
  }
}

/**
 * Haptics need no gesture but a visible widget and room in the bucket (5 calls,
 * refilled at 10 per second). `play` delivers; `drop` drops silently; `report`
 * drops and reports `haptic-limit` (at most once per second).
 */
export class HapticGate {
  private tokens = HAPTIC_BURST;
  private last: number | null = null;
  private reported: number | null = null;

  decide(now: number, visible: boolean): 'play' | 'drop' | 'report' {
    if (this.last !== null) this.tokens = Math.min(HAPTIC_BURST, this.tokens + (Math.max(0, now - this.last) * HAPTIC_PER_SECOND) / 1000);
    this.last = now;
    if (visible && this.tokens >= 1) {
      this.tokens -= 1;
      return 'play';
    }
    if (this.reported !== null && now - this.reported < HAPTIC_REPORT_MS) return 'drop';
    this.reported = now;
    return 'report';
  }
}

/** The theme keys of `params` whose values are `#rrggbb` (lowercased); everything else is dropped. */
export function checkThemeParams(params: unknown): Partial<ThemeParams> {
  const out: Partial<ThemeParams> = {};
  if (typeof params !== 'object' || params === null || Array.isArray(params)) return out;
  for (const key of THEME_KEYS) {
    const v = (params as Record<string, unknown>)[key];
    if (typeof v === 'string' && /^#[0-9a-fA-F]{6}$/.test(v)) out[key] = v.toLowerCase();
  }
  return out;
}

/** A BCP 47 tag of at most 35 characters (`ru`, `en-US`, `pt-BR`, `zh-Hant-TW`), or null. */
export function checkLocale(locale: unknown): string | null {
  if (typeof locale !== 'string' || locale.length === 0 || locale.length > MAX_LOCALE_LENGTH) return null;
  return /^[A-Za-z]{2,8}(-[A-Za-z0-9]{1,8})*$/.test(locale) ? locale : null;
}

/** UTF-8 bytes of an area's keys and values. */
export function storageBytes(area: Record<string, string>): number {
  let n = 0;
  for (const k of Object.keys(area)) n += utf8Length(k) + utf8Length(area[k]);
  return n;
}

/** An object of strings within the quota, or null. */
export function checkStorageArea(area: unknown): Record<string, string> | null {
  if (typeof area !== 'object' || area === null || Array.isArray(area)) return null;
  const out: Record<string, string> = {};
  for (const k of Object.keys(area)) {
    const v = (area as Record<string, unknown>)[k];
    if (typeof v !== 'string') return null;
    out[k] = v;
  }
  return storageBytes(out) <= STORAGE_QUOTA_BYTES ? out : null;
}

export type StorageCommit = { local?: Record<string, string>; session?: Record<string, string> };

/** A commit (or a preload) of `local` and/or `session`, each checked whole; null when anything is wrong (`storage-rejected`). */
export function checkStorageCommit(commit: unknown): StorageCommit | null {
  if (typeof commit !== 'object' || commit === null || Array.isArray(commit)) return null;
  const out: StorageCommit = {};
  for (const k of Object.keys(commit)) {
    if (k !== 'local' && k !== 'session') return null;
    const area = checkStorageArea((commit as Record<string, unknown>)[k]);
    if (area === null) return null;
    out[k] = area;
  }
  return out;
}

// ── theme defaults and renderer variables (TS only: the shim and the renderer core) ──

/** What a key the client did not pass falls back to. Clients pass every key (docs/sdk-v1.md §5.1). */
export const DEFAULT_THEME_PARAMS: Record<'light' | 'dark', ThemeParams> = {
  light: {
    bg_color: '#ffffff', text_color: '#000000', hint_color: '#707579', link_color: '#3390ec',
    button_color: '#3390ec', button_text_color: '#ffffff', secondary_bg_color: '#f4f4f5',
    accent_text_color: '#3390ec', subtitle_text_color: '#707579', destructive_text_color: '#e53935',
    section_bg_color: '#ffffff', section_header_text_color: '#3390ec', section_separator_color: '#e7e7e7',
    header_bg_color: '#ffffff', bottom_bar_bg_color: '#ffffff',
  },
  dark: {
    bg_color: '#212121', text_color: '#ffffff', hint_color: '#aaaaaa', link_color: '#8774e1',
    button_color: '#8774e1', button_text_color: '#ffffff', secondary_bg_color: '#0f0f0f',
    accent_text_color: '#8774e1', subtitle_text_color: '#aaaaaa', destructive_text_color: '#ff595a',
    section_bg_color: '#212121', section_header_text_color: '#8774e1', section_separator_color: '#3d3d3d',
    header_bg_color: '#212121', bottom_bar_bg_color: '#212121',
  },
};

export function completeThemeParams(params: unknown, scheme: 'light' | 'dark'): ThemeParams {
  return { ...DEFAULT_THEME_PARAMS[scheme], ...checkThemeParams(params) };
}

/** The client's own `<button>` (docs/sdk-v1.md §2.4); every field optional, the template's defaults stand for the rest. */
export interface ButtonTokens {
  bg?: string;
  text?: string;
  radius?: string;
  font?: string;
  padding?: string;
  height?: string;
  pressedOpacity?: number | string;
  disabledOpacity?: number | string;
}

/** What the host passes to the renderer: the colour scheme, the palette, the button tokens. */
export interface WidgetTheme {
  colorScheme: 'light' | 'dark';
  params?: Partial<ThemeParams>;
  button?: ButtonTokens;
}

const LENGTH = /^(0|[0-9]{1,4}(\.[0-9]{1,3})?(px|em|rem))$/;
const COLOR = /^#[0-9a-fA-F]{6}$/;
const GENERIC = new Set(['system-ui', 'sans-serif', 'serif', 'monospace', 'ui-sans-serif', 'ui-serif', 'ui-monospace', 'ui-rounded']);
const FONT = /^(?:(normal|italic) )?(?:(normal|bold|[1-9]00) )?[0-9]{1,3}(?:\.[0-9]{1,2})?(?:px|em|rem)(?:\/[0-9]{1,3}(?:\.[0-9]{1,2})?(?:px|em|rem)?)? ([a-z-]+(?:, ?[a-z-]+)*)$/;

function opacity(v: unknown): string | null {
  const s = typeof v === 'number' ? String(v) : typeof v === 'string' ? v : '';
  return /^(0(\.[0-9]{1,3})?|1(\.0{1,3})?|\.[0-9]{1,3})$/.test(s) ? s : null;
}

const BUTTON_VARS: [keyof ButtonTokens, string, (v: unknown) => string | null][] = [
  ['bg', '--tg-button-bg', (v) => (typeof v === 'string' && COLOR.test(v) ? v.toLowerCase() : null)],
  ['text', '--tg-button-text', (v) => (typeof v === 'string' && COLOR.test(v) ? v.toLowerCase() : null)],
  ['radius', '--tg-button-radius', (v) => (typeof v === 'string' && LENGTH.test(v) ? v : null)],
  ['font', '--tg-button-font', (v) => {
    const m = typeof v === 'string' ? FONT.exec(v) : null;
    return m && m[3].split(',').every((f) => GENERIC.has(f.trim())) ? (v as string) : null;
  }],
  ['padding', '--tg-button-padding', (v) => (typeof v === 'string' && /^\S+( \S+){0,3}$/.test(v) && v.split(' ').every((p) => LENGTH.test(p)) ? v : null)],
  ['height', '--tg-button-height', (v) => (typeof v === 'string' && LENGTH.test(v) ? v : null)],
  ['pressedOpacity', '--tg-button-pressed-opacity', opacity],
  ['disabledOpacity', '--tg-button-disabled-opacity', opacity],
];

/** Every variable name the renderer may set on the widget's root. */
export const THEME_VARIABLES: readonly string[] = [
  ...THEME_KEYS.map((k) => '--tg-theme-' + k.replace(/_/g, '-')),
  '--tg-color-scheme',
  ...BUTTON_VARS.map(([, name]) => name),
];

/** The CSS variables of a theme (docs/sdk-v1.md §2.3, §2.4, §3.4): names from the fixed list, values that match their grammar. */
export function themeVariables(theme: unknown): Record<string, string> {
  const t = (typeof theme === 'object' && theme !== null ? theme : {}) as Record<string, unknown>;
  const scheme = t.colorScheme === 'dark' ? 'dark' : 'light';
  const params = completeThemeParams(t.params, scheme);
  const vars: Record<string, string> = {};
  for (const k of THEME_KEYS) vars['--tg-theme-' + k.replace(/_/g, '-')] = params[k];
  vars['--tg-color-scheme'] = scheme;
  const button = (typeof t.button === 'object' && t.button !== null ? t.button : {}) as Record<string, unknown>;
  for (const [field, name, check] of BUTTON_VARS) {
    const v = check(button[field]);
    if (v !== null) vars[name] = v;
  }
  return vars;
}

/** A host's `theme` option: a plain scheme (the API before v1) or a WidgetTheme, with its palette checked. */
export function toWidgetTheme(theme: unknown): WidgetTheme & { params: Partial<ThemeParams> } {
  if (theme === 'light' || theme === 'dark') return { colorScheme: theme, params: {} };
  const t = (typeof theme === 'object' && theme !== null ? theme : {}) as Record<string, unknown>;
  const button = typeof t.button === 'object' && t.button !== null ? (t.button as ButtonTokens) : undefined;
  return { colorScheme: t.colorScheme === 'dark' ? 'dark' : 'light', params: checkThemeParams(t.params), ...(button ? { button } : {}) };
}
