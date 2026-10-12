// Entry point of the validator bundle used by native hosts: the same Validator
// as the web host, evaluated in a TRUSTED QuickJS context (one per app, never
// the widget's). The native side talks JSON strings only.
//
//   __twValidator.create(theme)             → creates the instance
//   __twValidator.processOps(json)          → json RenderOp[]
//   __twValidator.validateQuery(json)       → json Query or '' if rejected
//   __twValidator.validateListen(json)      → json string[]
//   __twValidator.validatePreventDefault(json) → json string[]
//   __twValidator.setTheme('light'|'dark')  → json RenderOp[] (re-emitted stylesheets)
//   __twValidator.rewriteMediaQuery(query)  → string
//   __twValidator.nodeCount()               → number
// Diagnostics are reported through `__diag(kind, detail)` if the host defines it.

import { Validator } from './validator';

declare const __diag: ((kind: string, detail: string) => void) | undefined;

let validator: Validator | null = null;
const diag = (kind: string, detail: string) => {
  if (typeof __diag === 'function') __diag(kind, detail);
};
const v = () => {
  if (!validator) throw new Error('validator not created');
  return validator;
};

(globalThis as any).__twValidator = {
  create(theme: 'light' | 'dark') {
    validator = new Validator(diag, { colorScheme: theme === 'dark' ? 'dark' : 'light' });
  },
  processOps(json: string) {
    return JSON.stringify(v().processOps(JSON.parse(json)));
  },
  validateQuery(json: string) {
    const q = v().validateQuery(JSON.parse(json));
    return q ? JSON.stringify(q) : '';
  },
  validateListen(json: string) {
    return JSON.stringify(v().validateListen(JSON.parse(json)));
  },
  validatePreventDefault(json: string) {
    return JSON.stringify(v().validatePreventDefault(JSON.parse(json)));
  },
  setTheme(theme: 'light' | 'dark') {
    return JSON.stringify(v().setTheme(theme === 'dark' ? 'dark' : 'light'));
  },
  rewriteMediaQuery(query: string) {
    return v().rewriteMediaQuery(String(query));
  },
  nodeCount() {
    return v().nodeCount;
  },
};
