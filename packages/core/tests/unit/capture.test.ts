// Reconciliation fast paths: a replaced sole text child keeps its id, the
// attribute removal scan, and the arity-specialized dirty-marking wrappers.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

const opsAfterBoot = async (html: string) => {
  const r = await runWidget(html, { timerRounds: 3, probes: ['window.__probe || ""'] });
  const boot = r.ops[0]?.length ?? 0;
  return { r, later: r.ops.slice(1).flat() as any[], boot };
};

describe('capture: sole text child', () => {
  it('textContent on an element with one text child sends one data op and keeps the node id', async () => {
    const { r, later } = await opsAfterBoot(`<span id="t">0</span><script>
      var t = document.getElementById('t'), n = 0;
      function tick() { t.textContent = String(++n); if (n < 3) setTimeout(tick, 1); }
      setTimeout(tick, 1);</script>`);
    assert.deepEqual(later.map((o) => `${o.op}:${o.data ?? ''}`), ['data:1', 'data:2', 'data:3']);
    const textId = r.ops[0].find((o: any) => o.op === 'text' && o.data === '0')?.id;
    assert.ok(later.every((o) => o.id === textId));
  });

  it('a MutationObserver still sees the replacement as a childList record', async () => {
    const r = await runWidget(`<span id="t">a</span><script>
      var t = document.getElementById('t'), seen = [];
      new MutationObserver(function (rs) { rs.forEach(function (x) { seen.push(x.type + ':' + x.addedNodes.length + ':' + x.removedNodes.length); }); }).observe(t, { childList: true });
      setTimeout(function () { t.textContent = 'b'; }, 1);</script>`, { timerRounds: 3, probes: ['seen.join()', 't.firstChild.data'] });
    assert.equal(r.probes[0], 'childList:1:1');
    assert.equal(r.probes[1], 'b');
  });

  it('falls back to rm and ins when the old text node was moved elsewhere', async () => {
    const { later } = await opsAfterBoot(`<span id="t">a</span><b id="o"></b><script>
      var t = document.getElementById('t'), o = document.getElementById('o');
      setTimeout(function () { var old = t.firstChild; o.appendChild(old); t.appendChild(document.createTextNode('b')); }, 1);</script>`);
    const kinds = later.map((o) => o.op).sort();
    assert.deepEqual(kinds, ['ins', 'ins', 'text']);
  });
});

describe('capture: attributes and wrappers', () => {
  it('removed attributes are still reported after the counting scan', async () => {
    const { later } = await opsAfterBoot(`<div id="d" a="1" b="2" c="3"></div><script>
      var d = document.getElementById('d');
      setTimeout(function () { d.removeAttribute('b'); d.setAttribute('a', '9'); }, 1);
      setTimeout(function () { d.setAttribute('e', '5'); d.removeAttribute('c'); }, 2);</script>`);
    assert.deepEqual(later.map((o) => `${o.op}:${o.name}${o.value !== undefined ? '=' + o.value : ''}`).sort(), ['attr:a=9', 'attr:e=5', 'rattr:b', 'rattr:c']);
  });

  it('wrapped DOM methods keep their results and argument counts', async () => {
    const r = await runWidget(`<ul id="l"></ul><script>
      var l = document.getElementById('l'), a = document.createElement('li'), b = document.createElement('li'), c = document.createElement('li');
      l.append(a, b, c); l.insertBefore(c, a); var removed = l.removeChild(b);
      l.prepend(); l.append('x', 'y', 'z');
      var out = [l.children.length, removed === b, l.firstChild === c, l.textContent].join();</script>`, { timerRounds: 0, probes: ['out'] });
    assert.equal(r.probes[0], '2,true,true,xyz');
  });

  it('a sloppy-mode caller of a wrapped method does not leak a mapped arguments object', async () => {
    const r = await runWidget(`<div id="d"></div><script>
      var d = document.getElementById('d'), k = 0;
      function add() { return d.appendChild.apply(d, arguments); }
      for (var i = 0; i < 2000; i++) { add(document.createElement('i')); if (d.childNodes.length > 50) d.textContent = ''; k++; }</script>`, { timerRounds: 0, probes: ['k'] });
    assert.equal(r.probes[0], '2000');
  });
});
