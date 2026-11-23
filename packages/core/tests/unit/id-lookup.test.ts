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

describe('shim: a className that changes nothing', () => {
  it('sends no op for one token, many or none, and still sends a change', async () => {
    const quiet = await runWidget(`<p id="a" class="x">a</p><p id="b" class="x y">b</p><p id="c" class="">c</p><script>
      var a = document.getElementById('a'), b = document.getElementById('b'), c = document.getElementById('c'), n = 0;
      function again() { a.className = 'x'; b.className = 'x y'; c.className = ''; if (++n < 5) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n', "[a.className, b.className, c.className, b.classList.length, b.classList.contains('y')].join('|')"] });
    assert.deepEqual(quiet.probes, ['5', 'x|x y||2|true']);
    assert.deepEqual(quiet.ops.slice(1).flat(), [], 'nothing goes to the host for the same class');
    const changed = await runWidget(`<p id="a" class="x">a</p><script>
      var a = document.getElementById('a'), n = 0;
      function again() { a.className = 'x'; a.className = n & 1 ? 'x' : 'x z'; if (++n < 4) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n', 'a.className', 'document.getElementsByClassName("z").length'] });
    assert.deepEqual(changed.probes, ['4', 'x', '0']);
    const values = changed.ops.flat().filter((o: any) => o.op === 'attr' && o.name === 'class').map((o: any) => o.value);
    assert.deepEqual(values.slice(-4), ['x z', 'x', 'x z', 'x']);
  });
});


describe('shim: writes that ride with a text or a class written again', () => {
  const colors = (r: any) => r.ops.flat().filter((o: any) => o.op === 'style' && o.name === 'color').map((o: any) => o.value);

  it('a style written with a new text in one task reaches the host', async () => {
    const r = await runWidget(`<span id="t" style="color: red">a</span><script>
      var t = document.getElementById('t'), n = 0, list = ['green', 'yellow', 'purple'];
      function again() { t.style.color = list[n]; t.textContent = 't' + n; if (++n < 3) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n', 't.style.color'] });
    assert.deepEqual(r.probes, ['3', 'purple']);
    assert.deepEqual(colors(r), ['green', 'yellow', 'purple']);
    const other = await runWidget(`<span id="t" style="color: red">a</span><script>
      var t = document.getElementById('t'), n = 0, list = ['green', 'yellow', 'purple'];
      function again() { t.textContent = 't' + n; t.style.color = list[n]; if (++n < 3) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n'] });
    assert.deepEqual(colors(other), ['green', 'yellow', 'purple']);
  });

  it('a style written with the class the element already has reaches the host', async () => {
    const r = await runWidget(`<span id="t" class="x y" style="color: red">a</span><script>
      var t = document.getElementById('t'), n = 0, list = ['green', 'yellow', 'purple'];
      function again() { t.style.color = list[n]; t.className = 'x y'; if (++n < 3) setTimeout(again, 1); }
      setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n', 't.style.color'] });
    assert.deepEqual(r.probes, ['3', 'purple']);
    assert.deepEqual(colors(r), ['green', 'yellow', 'purple']);
  });

  it('an attribute written through a kept Attr node goes with the text and with the class', async () => {
    for (const write of ["t.textContent = 't' + n", "t.className = 'x'"]) {
      const r = await runWidget(`<span id="t" class="x" aria-label="zero">a</span><script>
        var t = document.getElementById('t'), label = t.getAttributeNode('aria-label'), n = 0, list = ['one', 'two', 'three'];
        function again() { label.value = list[n]; ${write}; if (++n < 3) setTimeout(again, 1); }
        setTimeout(again, 1);</script>`, { timerRounds: 8, probes: ['n'] });
      const labels = r.ops.slice(1).flat().filter((o: any) => o.op === 'attr' && o.name === 'aria-label').map((o: any) => o.value);
      assert.deepEqual(labels, ['one', 'two', 'three'], write);
    }
  });
});

describe('shim: document.getElementById and className, the ways around the wrappers', () => {
  it('an earlier element given the id through an Attr node is the one found', async () => {
    const r = await runWidget(`<div id="r"><p id="e">early</p><p id="x">late</p></div><script>
      var out = [document.getElementById('x').textContent], early = document.getElementById('e');
      early.getAttributeNode('id').value = 'x'; out.push(document.getElementById('x').textContent);
      early.getAttributeNode('id').value = 'e'; out.push(document.getElementById('x').textContent);
      var res = out.join(',');
    </script>`, { probes: ['res'] });
    assert.equal(r.probes[0], 'late,early,late');
  });

  it('answers what a walk of the document answers after every way to change an id or the tree', async () => {
    const r = await runWidget(`<div id="r"><p id="p0">0</p><p id="p1">1</p><p id="p2">2</p><p id="p3">3</p></div><script>
      var r = document.getElementById('r'), keys = ['x', 'y', 'p0', 'p1', 'p2', 'p3', 'q'], bad = [], step = 0;
      function walk(id) { var all = document.getElementsByTagName('*'); for (var i = 0; i < all.length; i++) if (all[i].getAttribute('id') === id) return all[i]; return null; }
      function check(what) { step++; for (var i = 0; i < keys.length; i++) if (document.getElementById(keys[i]) !== walk(keys[i])) bad.push(what + ':' + keys[i]); }
      function kid(i) { return r.children[i]; }
      function attr(v) { var a = document.createAttribute('id'); a.value = v; return a; }
      check('start');
      r.appendChild(document.createElement('p')).setAttribute('id', 'x'); check('late x');
      kid(0).getAttributeNode('id').value = 'x'; check('Attr.value');
      kid(0).setAttributeNode(attr('y')); check('setAttributeNode');
      kid(4).attributes.setNamedItem(attr('y')); check('setNamedItem late');
      kid(1).attributes.setNamedItem(attr('x')); check('setNamedItem');
      kid(1).attributes.removeNamedItem('id'); check('removeNamedItem');
      kid(2).setAttributeNS(null, 'id', 'y'); check('setAttributeNS');
      kid(0).removeAttributeNode(kid(0).getAttributeNode('id')); check('removeAttributeNode');
      kid(3).toggleAttribute('id'); check('toggleAttribute');
      kid(2).removeAttributeNS(null, 'id'); check('removeAttributeNS');
      kid(4).id = 'q'; kid(0).id = 'q'; check('id');
      kid(0).attributes.id.value = 'p0'; check('attributes.id.value');
      kid(1).outerHTML = '<b id="q">b</b>'; check('outerHTML');
      kid(2).insertAdjacentElement('beforebegin', (function () { var e = document.createElement('i'); e.id = 'q'; return e; })()); check('insertAdjacentElement');
      var f = document.createDocumentFragment(), s = document.createElement('s'); s.id = 'p0'; f.appendChild(s); r.prepend(f); check('prepend fragment');
      kid(0).replaceWith(document.createElement('u')); check('replaceWith');
      var t = document.createElement('template'); t.innerHTML = '<em id="p0">e</em>'; r.insertBefore(t.content, r.firstChild); check('template content');
      r.firstChild.before((function () { var e = document.createElement('i'); e.id = 'p0'; return e; })()); check('before');
      r.replaceChild(document.createElement('u'), r.firstChild); check('replaceChild');
      var c = r.lastChild.cloneNode(true); r.insertBefore(c, r.firstChild); check('clone');
      r.textContent = 'gone'; check('textContent');
      var res = step + ' ' + bad.join(',');
    </script>`, { probes: ['res'] });
    assert.equal(r.probes[0], '22 ');
  });

  it('a class written past the token list is taken from className written after', async () => {
    const r = await runWidget(`<p id="a" class="old">a</p><p id="b" class="old">b</p><script>
      var a = document.getElementById('a'), b = document.getElementById('b'), la = a.classList, lb = b.classList;
      a.getAttributeNode('class').value = 'new two'; a.className = 'new two';
      b.getAttributeNode('class').value = 'solo'; b.className = 'solo';
      var res = [a.className, la.contains('new'), la.contains('old'), la.length, b.className, lb.contains('solo'), lb.contains('old')].join('|');
    </script>`, { probes: ['res'] });
    assert.equal(r.probes[0], 'new two|true|false|2|solo|true|false');
  });
});
