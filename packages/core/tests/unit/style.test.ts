// Inline style: the declaration parser's fast path (no quotes or escapes) against
// the character loop it short-cuts, and the style ops the shim emits across
// flushes now that parsed style texts are reused from the previous flush.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { parseDeclarations, toKebab, type Decl } from '../../src/sandbox/style.ts';
import { runWidget } from '../../scripts/sandbox-node.ts';

function reference(text: string): Map<string, Decl> {
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

const dump = (m: Map<string, Decl>) => JSON.stringify([...m]);

describe('style: declaration parser', () => {
  it('fast path matches the character loop on random declaration lists', () => {
    const pieces = ['color', 'Width', '--x', ':', ':', ';', ';', ' ', '(', ')', 'red', '1px', 'url(a;b)', '!important', '! IMPORTANT', '!', 'calc(1px + (2px))', '"q;"', "'", '\\;', 'a'];
    let seed = 1;
    const next = () => ((seed = (Math.imul(seed, 1103515245) + 12345) & 0x7fffffff) >>> 8);
    for (let n = 0; n < 20000; n++) {
      let text = '';
      const len = next() % 14;
      for (let i = 0; i < len; i++) text += pieces[next() % pieces.length];
      assert.equal(dump(parseDeclarations(text)), dump(reference(text)), JSON.stringify(text));
    }
  });

  it('keeps the CSSOM-facing results of real style texts', () => {
    const text = 'color: red; background: url(data:image/png;base64,AAAA) no-repeat; --gap: 4px; width: calc(100% - (2 * 4px)) !important';
    assert.equal(dump(parseDeclarations(text)), dump(reference(text)));
    assert.equal(parseDeclarations(text).get('width')?.important, true);
  });

  it('toKebab is stable through its cache', () => {
    for (let i = 0; i < 3; i++) {
      assert.equal(toKebab('backgroundColor'), 'background-color');
      assert.equal(toKebab('webkitTransform'), '-webkit-transform');
      assert.equal(toKebab('cssFloat'), 'float');
      assert.equal(toKebab('--myVar'), '--myVar');
    }
  });
});

describe('style: ops across flushes', () => {
  it('emits per-property diffs when a style text comes back from an earlier flush', async () => {
    const r = await runWidget(`<div id="b" style="color: red; width: 1px">x</div><script>
      var b = document.getElementById('b'), step = 0;
      var texts = ['color: blue; width: 1px', 'color: red; width: 1px', 'color: red', 'color: red; width: 1px; height: 2px'];
      function tick() { b.setAttribute('style', texts[step++]); if (step < texts.length) setTimeout(tick, 1); }
      setTimeout(tick, 1);</script>`, { timerRounds: 6 });
    const style = r.renderOps.filter((o: any) => o.op === 'style').map((o: any) => `${o.name}=${o.value}`);
    assert.deepEqual(style, ['color=blue', 'color=red', 'width=', 'width=1px', 'height=2px']);
  });

  it('element.style writes in a loop reach the host as the last value of each property', async () => {
    const r = await runWidget(`<div id="b">x</div><script>
      var b = document.getElementById('b'), n = 0;
      function tick() { for (var i = 0; i <= n; i++) { b.style.transform = 'translateX(' + i + 'px)'; b.style.opacity = String(i / 10); } if (++n < 4) setTimeout(tick, 1); }
      setTimeout(tick, 1);</script>`, { timerRounds: 6 });
    const style = r.renderOps.filter((o: any) => o.op === 'style').map((o: any) => `${o.name}=${o.value}`);
    assert.deepEqual(style.slice(-2), ['transform=translateX(3px)', 'opacity=0.3']);
  });
});
