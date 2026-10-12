// CSSStyleDeclaration for element.style. Replaces linkedom's, which splits on
// ';' and therefore breaks on data: URLs. The style attribute stays the single
// source of truth: every write re-serializes into it, and capture.ts turns the
// attribute change into per-property ops.

export interface Decl { value: string; important: boolean }

/** Split a declaration list, respecting quotes, parentheses and escapes. */
export function parseDeclarations(text: string): Map<string, Decl> {
  const out = new Map<string, Decl>();
  let depth = 0;
  let quote = '';
  let start = 0;
  const push = (end: number) => {
    const decl = text.slice(start, end);
    const colon = decl.indexOf(':');
    if (colon > 0) {
      const name = decl.slice(0, colon).trim();
      let value = decl.slice(colon + 1).trim();
      let important = false;
      const m = /!\s*important\s*$/i.exec(value);
      if (m) {
        important = true;
        value = value.slice(0, m.index).trim();
      }
      if (name && value) {
        const key = name.startsWith('--') ? name : name.toLowerCase();
        out.delete(key);
        out.set(key, { value, important });
      }
    }
    start = end + 1;
  };
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (ch === '\\') i++;
    else if (quote) {
      if (ch === quote) quote = '';
    } else if (ch === '"' || ch === "'") quote = ch;
    else if (ch === '(') depth++;
    else if (ch === ')') depth = Math.max(0, depth - 1);
    else if (ch === ';' && depth === 0) push(i);
  }
  push(text.length);
  return out;
}

export function serializeDeclarations(map: Map<string, Decl>): string {
  let out = '';
  for (const [name, d] of map) out += `${out ? ' ' : ''}${name}: ${d.value}${d.important ? ' !important' : ''};`;
  return out;
}

export function toKebab(prop: string): string {
  if (prop.startsWith('--') || prop.includes('-')) return prop;
  if (prop === 'cssFloat') return 'float';
  const k = prop.replace(/[A-Z]/g, (c) => '-' + c.toLowerCase());
  return /^(webkit|moz|ms|o)-/.test(k) ? '-' + k : k;
}

const styles = new WeakMap<object, any>();

class StyleImpl {
  _el: any;
  _text: string | null = null;
  _map = new Map<string, Decl>();
  constructor(el: any) {
    this._el = el;
  }
  _read(): Map<string, Decl> {
    const text = this._el.getAttribute('style') ?? '';
    if (text !== this._text) {
      this._text = text;
      this._map = parseDeclarations(text);
    }
    return this._map;
  }
  _write() {
    this._text = serializeDeclarations(this._map);
    this._el.setAttribute('style', this._text);
  }
  getPropertyValue(name: string): string {
    return this._read().get(toKebab(String(name)))?.value ?? '';
  }
  getPropertyPriority(name: string): string {
    return this._read().get(toKebab(String(name)))?.important ? 'important' : '';
  }
  setProperty(name: string, value: unknown, priority?: string) {
    const key = toKebab(String(name));
    const map = this._read();
    if (value === null || value === undefined || value === '') {
      if (!map.delete(key)) return;
    } else {
      const v = String(value).trim();
      const old = map.get(key);
      const important = priority === 'important';
      if (old && old.value === v && old.important === important) return;
      map.delete(key); // re-append: the latest declaration wins, like CSSOM
      map.set(key, { value: v, important });
    }
    this._write();
  }
  removeProperty(name: string): string {
    const key = toKebab(String(name));
    const map = this._read();
    const old = map.get(key)?.value ?? '';
    if (map.delete(key)) this._write();
    return old;
  }
  item(i: number): string {
    return [...this._read().keys()][i] ?? '';
  }
  get length(): number {
    return this._read().size;
  }
  get cssText(): string {
    return this._el.getAttribute('style') ?? '';
  }
  set cssText(v: string) {
    this._el.setAttribute('style', String(v));
  }
  [Symbol.iterator]() {
    return this._read().keys();
  }
}

const handler: ProxyHandler<StyleImpl> = {
  get(t, prop) {
    if (typeof prop === 'symbol' || prop in t) {
      const v = (t as any)[prop];
      return typeof v === 'function' ? v.bind(t) : v;
    }
    if (/^\d+$/.test(prop)) return t.item(Number(prop));
    return t.getPropertyValue(prop);
  },
  set(t, prop, value) {
    if (typeof prop === 'symbol') return true;
    if (prop === 'cssText') t.cssText = value;
    else if (!prop.startsWith('_')) t.setProperty(prop, value);
    else (t as any)[prop] = value;
    return true;
  },
  has: () => true,
};

export function styleOf(el: any): any {
  let s = styles.get(el);
  if (!s) styles.set(el, (s = new Proxy(new StyleImpl(el), handler)));
  return s;
}
