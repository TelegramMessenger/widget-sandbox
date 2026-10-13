// Form controls, tables and other element APIs linkedom does not provide.
//
// The live value of a control lives in the renderer. The sandbox keeps a mirror
// (STATE) that is updated from every event the renderer forwards, and pushes
// script-initiated changes back through `prop` ops.

import { callMethod, markDirty } from './capture';
import { document } from './dom';
import { unsupported } from './host';
import { elQuery } from './layout';

const activePointers = new Set<number>();
const pointerCaptures = new Map<number, any>();

export function pointerEventTarget(target: any, event: any): any {
  const id = Number(event.pointerId) || 1;
  if (event.type === 'pointerdown') {
    activePointers.add(id);
    pointerCaptures.delete(id);
  }
  if (['pointermove', 'pointerup', 'pointercancel'].includes(event.type)) {
    const captured = pointerCaptures.get(id);
    if (captured?.isConnected) return captured;
    pointerCaptures.delete(id);
  }
  return target;
}

export function finishPointerEvent(event: any) {
  if (event.type === 'pointerup' || event.type === 'pointercancel') {
    const id = Number(event.pointerId) || 1;
    activePointers.delete(id);
    pointerCaptures.delete(id);
  }
}

const STATE = Symbol('tw.state');
interface State { value?: string; checked?: boolean; selectedIndex?: number; indeterminate?: boolean; dirty: Set<string>; naturalWidth?: number; naturalHeight?: number; complete?: boolean }

function st(el: any): State {
  return el[STATE] ?? (Object.defineProperty(el, STATE, { value: { dirty: new Set() }, enumerable: false })[STATE] as State);
}
function setState(el: any, name: 'value' | 'checked' | 'selectedIndex' | 'indeterminate', value: any) {
  const s = st(el);
  (s as any)[name] = value;
  s.dirty.add(name);
  markDirty(el);
}

/** capture hook: emit `prop` ops for properties scripts have set. */
export function emitProps(el: any, id: number, push: (op: any) => void, isNew: boolean) {
  const s: State | undefined = el[STATE];
  if (!s) return;
  const names = isNew ? (['value', 'checked', 'selectedIndex', 'indeterminate'] as const).filter((n) => s[n] !== undefined) : [...s.dirty];
  for (const name of names) push({ op: 'prop', id, name, value: (s as any)[name] });
  s.dirty.clear();
}

const tag = (el: any) => el.localName;
const optionsOf = (select: any): any[] => [...select.querySelectorAll('option')];

function selectIndex(select: any): number {
  const s = st(select);
  if (s.selectedIndex !== undefined) return s.selectedIndex;
  const opts = optionsOf(select);
  const i = opts.findIndex((o) => o.hasAttribute('selected'));
  if (i >= 0) return i;
  return select.hasAttribute('multiple') || !opts.length ? -1 : 0;
}

function optionValue(o: any): string {
  return o.hasAttribute('value') ? o.getAttribute('value') : (o.textContent ?? '').trim().replace(/\s+/g, ' ');
}

function inputDefault(el: any): string {
  const attr = el.getAttribute('value');
  const type = (el.getAttribute('type') ?? 'text').toLowerCase();
  if (type === 'range') {
    const min = parseFloat(el.getAttribute('min') ?? '0') || 0;
    let max = parseFloat(el.getAttribute('max') ?? '100');
    if (isNaN(max)) max = 100;
    const v = attr === null ? (max < min ? min : min + (max - min) / 2) : Math.min(Math.max(parseFloat(attr) || 0, min), Math.max(max, min));
    return String(v);
  }
  if (attr !== null) return attr;
  if (type === 'color') return '#000000';
  if (type === 'checkbox' || type === 'radio') return 'on';
  return '';
}

function radioGroup(el: any): any[] {
  const name = el.getAttribute('name');
  if (!name) return [];
  const root = el.closest('form') ?? document;
  return [...root.querySelectorAll('input[type=radio]')].filter((r: any) => r !== el && r.getAttribute('name') === name);
}

/** Update the mirror from a renderer event (no ops are generated). */
export function applyEventState(el: any, ev: any) {
  const t = tag(el);
  if (t === 'input') {
    const s = st(el);
    if (ev.value !== undefined) s.value = ev.value;
    if (ev.checked !== undefined) {
      s.checked = ev.checked;
      if (ev.checked && (el.getAttribute('type') ?? '').toLowerCase() === 'radio') for (const r of radioGroup(el)) st(r).checked = false;
    }
  } else if (t === 'textarea') {
    if (ev.value !== undefined) st(el).value = ev.value;
  } else if (t === 'select') {
    if (ev.selectedIndex !== undefined) st(el).selectedIndex = ev.selectedIndex;
  } else if (t === 'img' && ev.type === 'load') {
    const s = st(el);
    s.naturalWidth = ev.naturalWidth;
    s.naturalHeight = ev.naturalHeight;
    s.complete = true;
  } else if (t === 'details' && ev.type === 'toggle') {
    if (ev.open) el.setAttribute('open', '');
    else el.removeAttribute('open');
  }
}

/** Is a checkbox/radio currently checked (mirror or default attribute)? */
export function isChecked(el: any): boolean {
  return st(el).checked ?? el.hasAttribute('checked');
}

export function controlValue(el: any): string {
  const t = tag(el);
  if (t === 'input') return st(el).value ?? inputDefault(el);
  if (t === 'textarea') return st(el).value ?? el.textContent ?? '';
  if (t === 'select') {
    const o = optionsOf(el)[selectIndex(el)];
    return o ? optionValue(o) : '';
  }
  if (t === 'option') return optionValue(el);
  return el.getAttribute('value') ?? '';
}

function formControls(form: any): any[] {
  return [...form.querySelectorAll('input, select, textarea, button, output, fieldset')];
}

export class FormData {
  _e: [string, any][] = [];
  constructor(form?: any) {
    if (!form) return;
    for (const el of formControls(form)) {
      const name = el.getAttribute('name');
      if (!name || el.hasAttribute('disabled')) continue;
      const t = tag(el);
      const type = (el.getAttribute('type') ?? '').toLowerCase();
      if (t === 'button' || t === 'fieldset' || t === 'output' || type === 'submit' || type === 'button' || type === 'reset' || type === 'image' || type === 'file') continue;
      if ((type === 'checkbox' || type === 'radio') && !isChecked(el)) continue;
      this._e.push([name, controlValue(el)]);
    }
  }
  append(k: string, v: any) { this._e.push([String(k), String(v)]); }
  set(k: string, v: any) { this.delete(k); this.append(k, v); }
  delete(k: string) { this._e = this._e.filter(([n]) => n !== k); }
  get(k: string) { return this._e.find(([n]) => n === k)?.[1] ?? null; }
  getAll(k: string) { return this._e.filter(([n]) => n === k).map(([, v]) => v); }
  has(k: string) { return this._e.some(([n]) => n === k); }
  forEach(cb: (v: any, k: string, fd: FormData) => void) { for (const [k, v] of this._e) cb(v, k, this); }
  entries() { return this._e.map((e) => [...e])[Symbol.iterator](); }
  keys() { return this._e.map(([k]) => k)[Symbol.iterator](); }
  values() { return this._e.map(([, v]) => v)[Symbol.iterator](); }
  [Symbol.iterator]() { return this.entries(); }
}

function define(proto: any, props: Record<string, PropertyDescriptor | Function>) {
  for (const [name, d] of Object.entries(props)) {
    Object.defineProperty(proto, name, typeof d === 'function' ? { value: d, writable: true, configurable: true } : { configurable: true, ...d });
  }
}

const reflectString = (attr: string, dflt = ''): PropertyDescriptor => ({
  get(this: any) { return this.getAttribute(attr) ?? dflt; },
  set(this: any, v: any) { this.setAttribute(attr, String(v)); },
});
const reflectBool = (attr: string): PropertyDescriptor => ({
  get(this: any) { return this.hasAttribute(attr); },
  set(this: any, v: any) { if (v) this.setAttribute(attr, ''); else this.removeAttribute(attr); },
});
const reflectNumber = (attr: string, dflt: number): PropertyDescriptor => ({
  get(this: any) { const n = parseInt(this.getAttribute(attr) ?? '', 10); return isNaN(n) ? dflt : n; },
  set(this: any, v: any) { this.setAttribute(attr, String(v)); },
});

function validity(el: any) {
  return elQuery(el, { q: 'validity' }, { valid: true, message: '' });
}

export function installForms(protoOf: (tagName: string) => any, HTMLElementProto: any, ElementProto: any) {
  // ── generic reflected attributes missing from linkedom ──
  const sample = document.createElement('div');
  const generic: Record<string, PropertyDescriptor> = {
    hidden: reflectBool('hidden'), title: reflectString('title'), lang: reflectString('lang'), dir: reflectString('dir'),
    tabIndex: reflectNumber('tabindex', -1), draggable: reflectBool('draggable'), disabled: reflectBool('disabled'),
    name: reflectString('name'), type: reflectString('type'), placeholder: reflectString('placeholder'),
    readOnly: reflectBool('readonly'), required: reflectBool('required'), multiple: reflectBool('multiple'),
    min: reflectString('min'), max: reflectString('max'), step: reflectString('step'), pattern: reflectString('pattern'),
    maxLength: reflectNumber('maxlength', -1), minLength: reflectNumber('minlength', -1),
    htmlFor: reflectString('for'), alt: reflectString('alt'), src: reflectString('src'), href: reflectString('href'),
    target: reflectString('target'), rel: reflectString('rel'), colSpan: reflectNumber('colspan', 1), rowSpan: reflectNumber('rowspan', 1),
    open: reflectBool('open'), contentEditable: reflectString('contenteditable', 'inherit'), accessKey: reflectString('accesskey'),
    autofocus: reflectBool('autofocus'), noValidate: reflectBool('novalidate'), role: reflectString('role'),
    ariaLabel: reflectString('aria-label'), ariaHidden: reflectString('aria-hidden'), ariaExpanded: reflectString('aria-expanded'),
    ariaSelected: reflectString('aria-selected'), ariaChecked: reflectString('aria-checked'), ariaPressed: reflectString('aria-pressed'),
  };
  for (const [name, d] of Object.entries(generic)) if (!(name in sample)) define(HTMLElementProto, { [name]: d });

  // ── methods that act in the renderer ──
  define(ElementProto, {
    focus(this: any) { callMethod(this, 'focus'); },
    blur(this: any) { callMethod(this, 'blur'); },
    scrollIntoView(this: any) { callMethod(this, 'scrollIntoView'); },
    scrollTo(this: any, x: any, y: any) { const o = typeof x === 'object' && x ? x : { left: x, top: y }; callMethod(this, 'scrollTo', [Number(o.left) || 0, Number(o.top) || 0]); },
    scrollBy(this: any, x: any, y: any) { const o = typeof x === 'object' && x ? x : { left: x, top: y }; callMethod(this, 'scrollBy', [Number(o.left) || 0, Number(o.top) || 0]); },
    setPointerCapture(this: any, id: number) {
      id = Number(id) || 0;
      if (!activePointers.has(id) || !this.isConnected) return;
      pointerCaptures.set(id, this);
      callMethod(this, 'setPointerCapture', [id]);
    },
    releasePointerCapture(this: any, id: number) {
      id = Number(id) || 0;
      if (pointerCaptures.get(id) !== this) return;
      pointerCaptures.delete(id);
      callMethod(this, 'releasePointerCapture', [id]);
    },
    hasPointerCapture(this: any, id: number) { return pointerCaptures.get(Number(id)) === this; },
    showPopover(this: any) { callMethod(this, 'showPopover'); },
    hidePopover(this: any) { callMethod(this, 'hidePopover'); },
    togglePopover(this: any) { callMethod(this, 'togglePopover'); },
    requestFullscreen() { unsupported('Element.requestFullscreen'); return Promise.reject(new Error('Fullscreen is not available')); },
    animate() {
      unsupported('Element.animate (Web Animations API)');
      const done = Promise.resolve();
      return { finished: done, ready: done, cancel() {}, finish() {}, play() {}, pause() {}, reverse() {}, onfinish: null, addEventListener() {}, removeEventListener() {}, playState: 'finished', currentTime: 0 };
    },
    getAnimations() { return []; },
    attachShadow(this: any) { unsupported('Element.attachShadow (shadow DOM is not rendered)'); throw new Error('NotSupportedError: shadow DOM is not available'); },
  });

  // ── input ──
  const input = protoOf('input');
  define(input, {
    value: { get(this: any) { return controlValue(this); }, set(this: any, v: any) { setState(this, 'value', v === null ? '' : String(v)); } },
    defaultValue: reflectString('value'),
    checked: { get(this: any) { return isChecked(this); }, set(this: any, v: any) { setState(this, 'checked', !!v); if (v && this.type === 'radio') for (const r of radioGroup(this)) st(r).checked = false; } },
    defaultChecked: reflectBool('checked'),
    indeterminate: { get(this: any) { return st(this).indeterminate ?? false; }, set(this: any, v: any) { setState(this, 'indeterminate', !!v); } },
    type: { get(this: any) { return (this.getAttribute('type') ?? 'text').toLowerCase(); }, set(this: any, v: any) { this.setAttribute('type', String(v)); } },
    valueAsNumber: {
      get(this: any) { const v = controlValue(this); return v === '' ? NaN : Number(v); },
      set(this: any, v: any) { setState(this, 'value', String(v)); },
    },
    valueAsDate: { get(this: any) { const v = controlValue(this); const d = new Date(v); return v && !isNaN(d.getTime()) ? d : null; } },
    files: { get() { return null; } },
    select(this: any) { callMethod(this, 'select'); },
    setSelectionRange(this: any, a: number, b: number) { callMethod(this, 'setSelectionRange', [Number(a) || 0, Number(b) || 0]); },
    selectionStart: { get(this: any) { return controlValue(this).length; }, set() {} },
    selectionEnd: { get(this: any) { return controlValue(this).length; }, set() {} },
    stepUp(this: any, n = 1) { setState(this, 'value', String((Number(controlValue(this)) || 0) + n * (Number(this.getAttribute('step')) || 1))); },
    stepDown(this: any, n = 1) { setState(this, 'value', String((Number(controlValue(this)) || 0) - n * (Number(this.getAttribute('step')) || 1))); },
  });

  // ── textarea ──
  define(protoOf('textarea'), {
    value: { get(this: any) { return controlValue(this); }, set(this: any, v: any) { setState(this, 'value', v === null ? '' : String(v)); } },
    defaultValue: { get(this: any) { return this.textContent; }, set(this: any, v: any) { this.textContent = v; } },
    select(this: any) { callMethod(this, 'select'); },
    setSelectionRange(this: any, a: number, b: number) { callMethod(this, 'setSelectionRange', [Number(a) || 0, Number(b) || 0]); },
    rows: reflectNumber('rows', 2), cols: reflectNumber('cols', 20),
  });

  // ── select / option ──
  define(protoOf('select'), {
    options: { get(this: any) { return optionsOf(this); } },
    length: { get(this: any) { return optionsOf(this).length; } },
    selectedIndex: { get(this: any) { return selectIndex(this); }, set(this: any, v: any) { setState(this, 'selectedIndex', Number(v) | 0); } },
    selectedOptions: { get(this: any) { const o = optionsOf(this)[selectIndex(this)]; return o ? [o] : []; } },
    value: {
      get(this: any) { return controlValue(this); },
      set(this: any, v: any) { setState(this, 'selectedIndex', optionsOf(this).findIndex((o) => optionValue(o) === String(v))); },
    },
    add(this: any, option: any, before?: any) { this.insertBefore(option, typeof before === 'number' ? optionsOf(this)[before] ?? null : before ?? null); },
    item(this: any, i: number) { return optionsOf(this)[i] ?? null; },
  });
  define(protoOf('option'), {
    value: { get(this: any) { return optionValue(this); }, set(this: any, v: any) { this.setAttribute('value', String(v)); } },
    text: { get(this: any) { return (this.textContent ?? '').trim(); }, set(this: any, v: any) { this.textContent = v; } },
    label: { get(this: any) { return this.getAttribute('label') ?? (this.textContent ?? '').trim(); }, set(this: any, v: any) { this.setAttribute('label', String(v)); } },
    index: { get(this: any) { const s = this.closest('select'); return s ? optionsOf(s).indexOf(this) : 0; } },
    selected: {
      get(this: any) { const s = this.closest('select'); return s ? optionsOf(s)[selectIndex(s)] === this : this.hasAttribute('selected'); },
      set(this: any, v: any) {
        const s = this.closest('select');
        if (!s) return v ? this.setAttribute('selected', '') : this.removeAttribute('selected');
        if (v) setState(s, 'selectedIndex', optionsOf(s).indexOf(this));
        else if (optionsOf(s)[selectIndex(s)] === this) setState(s, 'selectedIndex', s.hasAttribute('multiple') ? -1 : 0);
      },
    },
    defaultSelected: reflectBool('selected'),
  });

  // ── shared by all listed controls ──
  for (const t of ['input', 'textarea', 'select', 'button', 'output', 'fieldset']) {
    define(protoOf(t), {
      form: { get(this: any) { return this.closest('form'); } },
      labels: { get(this: any) { const id = this.getAttribute('id'); return [...document.querySelectorAll('label')].filter((l: any) => l.contains(this) || (id && l.getAttribute('for') === id)); } },
      validity: { get(this: any) { return validity(this); } },
      validationMessage: { get(this: any) { return validity(this).message ?? ''; } },
      willValidate: { get() { return true; } },
      checkValidity(this: any) { return validity(this).valid !== false; },
      reportValidity(this: any) { return validity(this).valid !== false; },
      setCustomValidity() { unsupported('setCustomValidity'); },
    });
  }
  define(protoOf('button'), {
    type: { get(this: any) { return (this.getAttribute('type') ?? 'submit').toLowerCase(); }, set(this: any, v: any) { this.setAttribute('type', String(v)); } },
    value: reflectString('value'),
  });
  define(protoOf('label'), { control: { get(this: any) { const f = this.getAttribute('for'); return f ? document.getElementById(f) : this.querySelector('input, select, textarea, button'); } } });
  for (const t of ['progress', 'meter']) {
    define(protoOf(t), {
      value: { get(this: any) { return Number(this.getAttribute('value') ?? 0); }, set(this: any, v: any) { this.setAttribute('value', String(v)); } },
      max: { get(this: any) { return Number(this.getAttribute('max') ?? 1); }, set(this: any, v: any) { this.setAttribute('max', String(v)); } },
    });
  }
  define(protoOf('output'), { value: { get(this: any) { return this.textContent; }, set(this: any, v: any) { this.textContent = v; } } });

  // ── form ──
  define(protoOf('form'), {
    elements: {
      get(this: any) {
        const list: any = formControls(this);
        for (const el of list) {
          const n = el.getAttribute('name') || el.getAttribute('id');
          if (n && !(n in list)) list[n] = el;
        }
        list.namedItem = (n: string) => list[n] ?? null;
        return list;
      },
    },
    length: { get(this: any) { return formControls(this).length; } },
    reset(this: any) {
      for (const el of formControls(this)) {
        const s: State | undefined = el[STATE];
        if (s) { s.value = s.checked = s.selectedIndex = undefined; s.dirty.clear(); }
      }
      callMethod(this, 'reset');
      this.dispatchEvent(new (globalThis as any).Event('reset', { bubbles: true }));
    },
    submit() { unsupported('form.submit() (navigation is disabled; handle the submit event instead)'); },
    requestSubmit(this: any, submitter?: any) { this.dispatchEvent(new (globalThis as any).SubmitEvent('submit', { bubbles: true, cancelable: true, submitter })); },
    checkValidity(this: any) { return formControls(this).every((el: any) => typeof el.checkValidity !== 'function' || el.checkValidity()); },
    reportValidity(this: any) { return this.checkValidity(); },
    action: reflectString('action'), method: reflectString('method', 'get'),
  });

  // ── details / dialog ──
  define(protoOf('dialog'), {
    showModal(this: any) { this.setAttribute('open', ''); callMethod(this, 'showModal'); },
    show(this: any) { this.setAttribute('open', ''); },
    close(this: any, value?: string) {
      if (value !== undefined) this.returnValue = value;
      this.removeAttribute('open');
      callMethod(this, 'close');
      this.dispatchEvent(new (globalThis as any).Event('close'));
    },
    returnValue: { value: '', writable: true },
  });

  // ── img ──
  define(protoOf('img'), {
    naturalWidth: { get(this: any) { return st(this).naturalWidth ?? 0; } },
    naturalHeight: { get(this: any) { return st(this).naturalHeight ?? 0; } },
    complete: { get(this: any) { return st(this).complete ?? !this.getAttribute('src'); } },
    decode() { return Promise.resolve(); },
  });

  // ── tables ──
  const rowsOf = (root: any): any[] => {
    const out: any[] = [];
    for (const c of root.children) {
      if (c.localName === 'tr') out.push(c);
      else if (['thead', 'tbody', 'tfoot'].includes(c.localName)) for (const r of c.children) if (r.localName === 'tr') out.push(r);
    }
    return out;
  };
  const insertRow = function (this: any, index = -1) {
    const tr = document.createElement('tr');
    const rows = rowsOf(this);
    let section = this;
    if (this.localName === 'table') section = [...this.children].filter((c: any) => c.localName === 'tbody').pop() ?? this.appendChild(document.createElement('tbody'));
    if (index < 0 || index >= rows.length) section.appendChild(tr);
    else rows[index].parentNode.insertBefore(tr, rows[index]);
    return tr;
  };
  const deleteRow = function (this: any, index: number) {
    const rows = rowsOf(this);
    rows[index < 0 ? rows.length + index : index]?.remove();
  };
  const section = (name: string) => function (this: any) {
    let s = [...this.children].find((c: any) => c.localName === name);
    if (!s) {
      s = document.createElement(name);
      if (name === 'thead') this.insertBefore(s, this.firstChild);
      else this.appendChild(s);
    }
    return s;
  };
  define(protoOf('table'), {
    rows: { get(this: any) { return rowsOf(this); } },
    tBodies: { get(this: any) { return [...this.children].filter((c: any) => c.localName === 'tbody'); } },
    tHead: { get(this: any) { return [...this.children].find((c: any) => c.localName === 'thead') ?? null; } },
    tFoot: { get(this: any) { return [...this.children].find((c: any) => c.localName === 'tfoot') ?? null; } },
    caption: { get(this: any) { return [...this.children].find((c: any) => c.localName === 'caption') ?? null; } },
    insertRow, deleteRow, createTHead: section('thead'), createTBody: section('tbody'), createTFoot: section('tfoot'),
  });
  for (const t of ['thead', 'tbody', 'tfoot']) define(protoOf(t), { rows: { get(this: any) { return rowsOf(this); } }, insertRow, deleteRow });
  define(protoOf('tr'), {
    cells: { get(this: any) { return [...this.children].filter((c: any) => c.localName === 'td' || c.localName === 'th'); } },
    rowIndex: { get(this: any) { const t = this.closest('table'); return t ? rowsOf(t).indexOf(this) : -1; } },
    sectionRowIndex: { get(this: any) { return this.parentNode ? rowsOf(this.parentNode).indexOf(this) : -1; } },
    insertCell(this: any, index = -1) {
      const td = document.createElement('td');
      const cells = this.cells;
      this.insertBefore(td, index < 0 || index >= cells.length ? null : cells[index]);
      return td;
    },
    deleteCell(this: any, index: number) { const cells = this.cells; cells[index < 0 ? cells.length + index : index]?.remove(); },
  });
  for (const t of ['td', 'th']) define(protoOf(t), { cellIndex: { get(this: any) { return this.parentNode ? this.parentNode.cells.indexOf(this) : -1; } } });
}

/** Toggle state for a script-initiated el.click() on a checkbox/radio (the renderer is told via prop op). */
export function syntheticToggle(el: any): boolean {
  if (tag(el) !== 'input') return false;
  const type = (el.getAttribute('type') ?? '').toLowerCase();
  if (type === 'checkbox') el.checked = !el.checked;
  else if (type === 'radio') el.checked = true;
  else return false;
  return true;
}
