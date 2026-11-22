// document.getElementById keeps what it found until the tree or an `id` changes, so a widget that
// looks its elements up by id every frame does not walk the document for each call. What it answers
// is what a walk of the document answers, after every kind of change.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

describe('shim: document.getElementById', () => {
  it('follows inserts, removals, moves and every way to write an id', async () => {
    const r = await runWidget(`<div id="r"><p id="a">1</p><p id="b">2</p></div><script>
      var $ = function (id) { var e = document.getElementById(id); return e ? e.tagName + ':' + e.textContent : 'null'; }, out = [];
      var r = document.getElementById('r'), a = document.getElementById('a'), b = document.getElementById('b');
      out.push($('a'), $('b'), $('none'));
      a.id = 'c'; out.push($('a'), $('c'));
      a.setAttribute('id', 'a'); out.push($('a'), $('c'));
      b.removeAttribute('id'); out.push($('b'));
      b.setAttribute('id', 'b'); out.push($('b'));
      b.getAttributeNode('id').value = 'd'; out.push($('b'), $('d'));
      r.removeChild(a); out.push($('a'));
      r.appendChild(a); out.push($('a'));
      var e = document.createElement('i'); e.id = 'e'; e.textContent = 'x'; out.push($('e'));
      r.appendChild(e); out.push($('e'));
      e.remove(); out.push($('e'));
      r.innerHTML = '<u id="a">3</u>'; out.push($('a'), $('d'));
      r.insertAdjacentHTML('beforeend', '<s id="z">4</s>'); out.push($('z'));
      r.replaceChildren(); out.push($('a'), $('z'));
      var res = out.join(',');
    </script>`, { probes: ['res'] });
    assert.equal(r.probes[0], 'P:1,P:2,null,null,P:1,P:1,null,null,P:2,null,P:2,null,P:1,null,I:x,null,U:3,null,S:4,null,null');
  });

  it('answers the first element in tree order of two with one id', async () => {
    const r = await runWidget(`<div id="r"><p id="x">late</p></div><script>
      var r = document.getElementById('r'), out = [document.getElementById('x').textContent];
      var early = document.createElement('p'); early.id = 'x'; early.textContent = 'early';
      r.insertBefore(early, r.firstChild); out.push(document.getElementById('x').textContent);
      r.removeChild(early); out.push(document.getElementById('x').textContent);
      r.appendChild(early); out.push(document.getElementById('x').textContent);
      var res = out.join(',');
    </script>`, { probes: ['res'] });
    assert.equal(r.probes[0], 'late,early,late,late');
  });

  it('is not a walk of the document for each call, across tasks that write text, styles and classes', async () => {
    const r = await runWidget(`<div id="list"></div><script>
      var list = document.getElementById('list');
      for (var i = 0; i < 200; i++) { var d = document.createElement('div'); d.id = 'n' + i; d.textContent = 'x'; list.appendChild(d); }
      var reads = 0, proto = Element.prototype, own = proto;
      while (own && !Object.getOwnPropertyDescriptor(own, 'id')) own = Object.getPrototypeOf(own);
      var d = Object.getOwnPropertyDescriptor(own, 'id');
      Object.defineProperty(own, 'id', { configurable: true, get: function () { reads++; return d.get.call(this); }, set: d.set });
      var round = 0, last = '';
      function frame() {
        for (var i = 0; i < 20; i++) {
          var el = document.getElementById('n' + (i * 10));
          el.textContent = 'r' + round; el.style.transform = 'translateY(' + round + 'px)'; el.className = round & 1 ? 'a' : 'b';
          last = el.id;
        }
        if (++round < 10) setTimeout(frame, 1);
      }
      setTimeout(frame, 1);
    </script>`, { timerRounds: 14, probes: ['reads', 'round', 'last'] });
    assert.equal(r.probes[1], '10');
    assert.equal(r.probes[2], 'n190');
    // 200 lookups in a document of 200 elements: a walk each would read some 20,000 ids.
    assert.ok(Number(r.probes[0]) < 3000, `the document was walked again: ${r.probes[0]} id reads for 200 lookups`);
  });

  it('a fragment and a detached tree are still searched as they are', async () => {
    const r = await runWidget(`<p id="a">doc</p><script>
      var f = document.createDocumentFragment(), p = document.createElement('p'); p.id = 'a'; p.textContent = 'frag'; f.appendChild(p);
      var res = [f.getElementById('a').textContent, document.getElementById('a').textContent, f.getElementById('b')].join(',');
    </script>`, { probes: ['res'] });
    assert.equal(r.probes[0], 'frag,doc,');
  });
});

describe('shim: a textContent that changes nothing', () => {
  it('sends no op, and a new text sends one data op', async () => {
    const quiet = await runWidget(`<span id="t">a</span><script>
      var t = document.getElementById('t'), n = 0;
      function again() { t.textContent = 'a'; if (++n < 5) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n', 't.textContent'] });
    const changed = await runWidget(`<span id="t">a</span><script>
      var t = document.getElementById('t'), n = 0;
      function again() { t.textContent = 'a' + n; if (++n < 5) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n', 't.textContent'] });
    assert.deepEqual(quiet.probes, ['5', 'a']);
    assert.deepEqual(changed.probes, ['5', 'a4']);
    const data = (r: any) => r.ops.flat().filter((o: any) => o.op === 'data').length;
    const later = (r: any) => r.ops.slice(1).flat().map((o: any) => o.op);
    assert.equal(data(quiet), 0);
    assert.deepEqual(later(quiet), [], 'nothing goes to the host for the same text');
    assert.equal(data(changed), 5);
  });
});
