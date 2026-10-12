// Regression tests for the in-sandbox shim, run through the real QuickJS bundle
// with the Node harness (scripts/sandbox-node.ts). Each case is a bug found by
// the compat or attack suites and fixed generally in the shim.

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

const elementsOf = (r: Awaited<ReturnType<typeof runWidget>>) => r.renderOps.filter((o) => o.op === 'el').length;
const errorsOf = (r: Awaited<ReturnType<typeof runWidget>>) => r.logs.filter((l) => l.level === 'error').map((l) => l.text);

describe('shim: scripts', () => {
  it('does not execute scripts inserted through innerHTML / insertAdjacentHTML (no runaway recursion)', async () => {
    const r = await runWidget(`<p>x</p><script>document.body.innerHTML += '<i>after</i>';</script>`, { probes: ['document.querySelectorAll("i").length', 'document.querySelectorAll("script").length'] });
    assert.deepEqual(errorsOf(r), []);
    assert.ok(elementsOf(r) < 20, `element explosion: ${elementsOf(r)} elements`);
    assert.equal(r.probes[0], '1');
    const r2 = await runWidget(`<div id="d"></div><script>var n = 0; document.getElementById('d').innerHTML = '<script>n = 1<\\/script>'; document.getElementById('d').insertAdjacentHTML('beforeend', '<script>n = 2<\\/script>');</script>`, { probes: ['n'] });
    assert.equal(r2.probes[0], '0');
  });
  it('executes scripts created with createElement and dispatches load/error for src scripts', async () => {
    const r = await runWidget(`<script>var ran = 0; var s = document.createElement('script'); s.textContent = 'ran = 1'; document.body.appendChild(s);
      var e = document.createElement('script'); e.src = 'https://example.invalid/x.js'; e.onerror = function () { ran += 10; }; document.body.appendChild(e);</script>`, { probes: ['ran'] });
    assert.equal(r.probes[0], '11');
    assert.ok(r.diags.some((d) => d.kind === 'script-skipped'));
  });
});

describe('shim: globals', () => {
  it('lets top-level var declarations replace window aliases (frames, top, parent)', async () => {
    const r = await runWidget(`<script>var frames = 0; frames++; var top = 'x';</script>`, { probes: ['frames', 'top'] });
    assert.deepEqual(r.probes, ['1', 'x']);
  });
  it('network APIs throw NetworkDisabledError and are recorded', async () => {
    const r = await runWidget(`<script>var names = []; try { new WebSocket('ws://x') } catch (e) { names.push(e.name) } try { new XMLHttpRequest() } catch (e) { names.push(e.name) }
      fetch('http://x').catch(function (e) { names.push(e.name) });</script>`, { probes: ['names.join(",")'] });
    assert.equal(r.probes[0], 'NetworkDisabledError,NetworkDisabledError,NetworkDisabledError');
    assert.ok(r.diags.filter((d) => d.kind === 'network-blocked').length >= 3);
  });
});

describe('shim: selectors and forms', () => {
  it(':checked reflects live state, including :not() and :has()', async () => {
    const r = await runWidget(`<label><input type="checkbox" id="a"></label><input type="radio" name="g" id="b"><input type="radio" name="g" id="c" checked>
      <script>document.getElementById('a').checked = true; document.getElementById('b').checked = true;</script>`, {
      probes: ['[...document.querySelectorAll(":checked")].map(e => e.id).join(",")', 'document.querySelectorAll("input:not(:checked)").length', 'document.querySelectorAll("label:has(:checked)").length', 'document.getElementById("c").checked'],
    });
    assert.deepEqual(r.probes, ['a,b', '1', '1', 'false']);
  });
  it('FormData and select/option state follow events from the renderer', async () => {
    const r = await runWidget(`<form id="f"><input name="q" value="init"><select name="s"><option value="1">a</option><option value="2">b</option></select></form>`, {
      events: [{ type: 'input', targetId: 0, value: 'typed' }],
      probes: ['new FormData(document.getElementById("f")).get("q")', 'document.querySelector("select").value'],
    });
    // targetId 0 is unknown and ignored; the default values must survive.
    assert.deepEqual(r.probes, ['init', '1']);
  });
});

describe('shim: runtime limits', () => {
  it('throws RangeError beyond maxTimers, reports timer-limit once, and earlier timers still fire', async () => {
    const r = await runWidget(`<script>var fired = 0, err = null; for (var i = 0; i < 8; i++) setTimeout(function () { fired++; }, 1);
      try { setTimeout(function () { fired += 100; }, 1); } catch (e) { err = e.name; }
      try { setInterval(function () {}, 10); } catch (e) { err += ',' + e.name; }</script>`, { maxTimers: 8, timerRounds: 1, probes: ['err', 'fired'] });
    assert.deepEqual(r.probes, ['RangeError,RangeError', '8']);
    assert.equal(r.diags.filter((d) => d.kind === 'timer-limit').length, 1);
    assert.equal(r.diags.filter((d) => d.kind === 'timer-limit-enforced').length, 0, 'the glue backstop must not be needed');
    assert.equal(r.timers, 8);
  });
  it('frees a slot when a timer fires or is cleared', async () => {
    const r = await runWidget(`<script>var ok = 0; var a = setTimeout(function () {}, 1); var b = setTimeout(function () {}, 1); clearTimeout(a);
      try { setTimeout(function () { ok++; }, 1); } catch (e) { ok = -1; }</script>`, { maxTimers: 2, timerRounds: 1, probes: ['ok'] });
    assert.equal(r.probes[0], '1');
  });
});

describe('shim: visibility', () => {
  it('flips document.hidden / visibilityState and fires visibilitychange on the host signal', async () => {
    const r = await runWidget(`<script>var n = 0; document.addEventListener('visibilitychange', function () { n++; });</script>`, {
      script: [{ probe: 'document.visibilityState + "," + document.hidden' }, { visible: false }, { probe: 'document.visibilityState + "," + document.hidden + "," + n' }, { visible: false }, { visible: true }, { probe: 'document.visibilityState + "," + n' }],
    });
    assert.deepEqual(r.probes, ['visible,false', 'hidden,true,1', 'visible,2']);
  });
});

describe('harness: cpu quota', () => {
  it('defers timers once the widget has burnt its quota, but still runs events', async () => {
    // Each timer tick burns ~40 ms (under the 50 ms floor); with a 100 ms/s quota the fourth tick and the frame are deferred.
    const r = await runWidget(`<script>var ticks = 0; function burn() { var t = performance.now(); while (performance.now() - t < 40) {} ticks++; }
      for (var i = 0; i < 4; i++) setTimeout(burn, 1);</script>`, { cpuQuotaMs: 100, timerRounds: 1, script: [{ probe: 'ticks' }] });
    assert.equal(r.probes[0], '3');
    assert.equal(r.throttled, 2, 'the fourth timer and the frame are deferred');
    assert.ok(r.diags.some((d) => d.kind === 'cpu-throttled'));
    const r2 = await runWidget(`<script>var t = performance.now(); while (performance.now() - t < 150) {} var clicks = 0; document.addEventListener('click', function () { clicks++; });</script>`, {
      cpuQuotaMs: 100, timerRounds: 0, events: [{ type: 'click', targetId: 2 }], probes: ['clicks'],
    });
    assert.equal(r2.probes[0], '1', 'events run even with the quota spent');
    assert.equal(r2.throttled, 0);
  });
});
