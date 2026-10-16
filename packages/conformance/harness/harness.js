// Conformance harness, inlined into every widget of packages/conformance by
// scripts/conformance/build.ts (between the conformance:harness markers).
// Plain ES2017 so it runs in QuickJS (sandbox), a browser (native reference)
// and any renderer's host. The result is published as JSON in the
// `data-conformance` attribute of <html>, which every host can read from the
// validated ops (an `attr` op on the element the `el` op adopted as html):
//
//   {"v":1,"id":"auto/04-layout","mode":"sandbox","status":"running|awaiting|done",
//    "n":{"pass":3,"fail":1,"xfail":0,"xpass":0,"skip":0,"pending":0},
//    "checks":[["css.display.flex","row places children side by side","pass",""], …]}
//
// A check names one catalogue ID. Status: pass, fail, xfail (failed as
// expected: the feature is not implemented yet), xpass (expected to fail but
// passed), skip (not applicable here), pending (waiting for input or time).
var T = (function () {
  'use strict';
  var root = document.documentElement;
  var ID = root.getAttribute('data-test') || 'unknown';
  var sandbox = typeof TelegramWidget !== 'undefined';
  var mode = window.__conformanceMode || (sandbox ? 'sandbox' : 'native');
  var checks = [];
  var byKey = {};
  var started = false, finished = false, awaiting = false, deadline = 0;
  var queue = [];
  var panelTimer = 0;
  var targets = {}, targetRects = null, stageRects = null;

  function fmt(v) {
    if (typeof v === 'number') return String(Math.round(v * 100) / 100);
    if (typeof v === 'string') return JSON.stringify(v.length > 60 ? v.slice(0, 60) + '…' : v);
    try { return JSON.stringify(v); } catch (e) { return String(v); }
  }

  function record(id, name, ok, detail, opts) {
    opts = opts || {};
    var st;
    if (ok === null) st = 'skip';
    else if (ok === 'pending') st = 'pending';
    else if (opts.xfail) st = ok ? 'xpass' : 'xfail';
    else st = ok ? 'pass' : 'fail';
    var key = id + '|' + name;
    var c = byKey[key];
    if (!c) { c = byKey[key] = { id: id, name: name }; checks.push(c); }
    c.status = st;
    c.detail = detail ? String(detail).slice(0, 160) : '';
    if (opts.xfail && (st === 'xfail' || st === 'xpass')) c.detail = (c.detail ? c.detail + '; ' : '') + 'expected to fail: ' + opts.xfail;
    schedulePublish();
    return st;
  }

  /** Run fn now (or after start); fn returns true/false, [bool, detail], or null to skip. Exceptions fail the check. */
  function check(id, name, fn, opts) {
    opts = opts || {};
    if (opts.sandboxOnly && !sandbox) return record(id, name, null, 'sandbox only', opts);
    if (opts.nativeOnly && sandbox) return record(id, name, null, 'native only', opts);
    var r;
    try { r = typeof fn === 'function' ? fn() : fn; } catch (e) { return record(id, name, false, 'threw ' + (e && e.name ? e.name + ': ' : '') + (e && e.message), opts); }
    if (r === null || r === undefined) return record(id, name, null, 'not applicable', opts);
    if (Array.isArray(r)) return record(id, name, !!r[0], r[1], opts);
    return record(id, name, !!r, '', opts);
  }

  function eq(id, name, actual, expected, opts) {
    return check(id, name, function () { return [actual === expected, 'got ' + fmt(actual) + ', want ' + fmt(expected)]; }, opts);
  }
  function near(id, name, actual, expected, tol, opts) {
    return check(id, name, function () { return [typeof actual === 'number' && Math.abs(actual - expected) <= tol, 'got ' + fmt(actual) + ', want ' + fmt(expected) + ' ±' + tol]; }, opts);
  }

  /** A check resolved later (an event, a timer): pending until pass()/fail() or the deadline. */
  function wait(id, name, opts) {
    opts = opts || {};
    if (opts.sandboxOnly && !sandbox) { record(id, name, null, 'sandbox only', opts); return { pass: function () {}, fail: function () {}, done: true }; }
    record(id, name, 'pending', opts.hint || '', opts);
    var h = {
      done: false,
      pass: function (d) { if (!h.done) { h.done = true; record(id, name, true, d, opts); maybeFinish(); } },
      fail: function (d) { if (!h.done) { h.done = true; record(id, name, false, d, opts); maybeFinish(); } },
      result: function (ok, d) { ok ? h.pass(d) : h.fail(d); },
      skip: function (d) { if (!h.done) { h.done = true; record(id, name, null, d, opts); maybeFinish(); } },
    };
    return h;
  }

  function rect(el) {
    if (typeof el === 'string') el = document.querySelector(el);
    var r = el.getBoundingClientRect();
    return { x: r.left, y: r.top, w: r.width, h: r.height, r: r.right, b: r.bottom };
  }
  function cs(el, prop, pseudo) {
    if (typeof el === 'string') el = document.querySelector(el);
    return getComputedStyle(el, pseudo || null).getPropertyValue(prop).trim();
  }
  /** Normalised computed colour "r,g,b,a" (a 0–1, two decimals) from rgb()/rgba(). */
  function color(el, prop, pseudo) {
    var v = cs(el, prop || 'background-color', pseudo);
    var m = /rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:\s*[,/]\s*([\d.]+%?))?\s*\)/.exec(v);
    if (!m) return v;
    var a = m[4] === undefined ? 1 : m[4].slice(-1) === '%' ? parseFloat(m[4]) / 100 : parseFloat(m[4]);
    return Math.round(+m[1]) + ',' + Math.round(+m[2]) + ',' + Math.round(+m[3]) + ',' + Math.round(a * 100) / 100;
  }

  function counts() {
    var n = { pass: 0, fail: 0, xfail: 0, xpass: 0, skip: 0, pending: 0 };
    for (var i = 0; i < checks.length; i++) n[checks[i].status]++;
    return n;
  }

  function status() {
    if (finished) return 'done';
    if (!started) return 'running';
    var n = counts();
    if (queue.length) return 'running';
    if (n.pending === 0) return 'done';
    return awaiting ? 'awaiting' : 'running';
  }

  function publish() {
    panelTimer = 0;
    var st = status();
    if (st === 'done') finished = true;
    var n = counts();
    var out = { v: 1, id: ID, mode: mode, status: st, n: n, checks: checks.map(function (c) { return [c.id, c.name, c.status, c.detail]; }) };
    // Interactive widgets: targets are re-measured on every publish, so a runner taps where things are now.
    if (targetRects && st !== 'done') { try { measureTargets(); } catch (e) {} }
    if (targetRects) out.targets = targetRects;
    if (stageRects) out.stages = stageRects;
    root.setAttribute('data-conformance', JSON.stringify(out));
    var p = document.getElementById('conf-summary');
    if (p) {
      p.textContent = (st === 'done' ? '' : st + ' · ') + 'pass ' + n.pass + ' · fail ' + n.fail + (n.xfail ? ' · xfail ' + n.xfail : '') + (n.xpass ? ' · XPASS ' + n.xpass : '') + (n.skip ? ' · skip ' + n.skip : '') + (n.pending ? ' · pending ' + n.pending : '');
      p.className = n.fail || n.xpass ? 'bad' : n.pending ? '' : 'good';
    }
    var list = document.getElementById('conf-list');
    if (list) {
      var lines = [];
      for (var i = 0; i < checks.length; i++) {
        var c = checks[i];
        if (list.hasAttribute('data-all') || c.status === 'fail' || c.status === 'xpass' || c.status === 'pending')
          lines.push({ 'pass': '✓ ', 'fail': '✗ ', 'xfail': '· ', 'xpass': '! ', 'skip': '– ', 'pending': '… ' }[c.status] + c.id + ' — ' + c.name + (c.detail && c.status !== 'pass' ? ' (' + c.detail + ')' : ''));
      }
      list.textContent = lines.join('\n');
    }
    if (st === 'done' && typeof console !== 'undefined') console.log('[conformance] ' + ID + ' ' + JSON.stringify(n));
  }
  function schedulePublish() { if (!panelTimer) panelTimer = setTimeout(publish, 0); }

  function maybeFinish() {
    if (!started || queue.length) return;
    var n = counts();
    if (n.pending === 0) { finished = true; publish(); }
    else schedulePublish();
  }

  /** Mark the remaining pending checks failed (no event arrived) and finish. */
  function timeout() {
    for (var i = 0; i < checks.length; i++) if (checks[i].status === 'pending') { checks[i].status = 'fail'; checks[i].detail = (checks[i].detail ? checks[i].detail + '; ' : '') + 'timed out'; }
    finished = true;
    publish();
  }

  /** Steps run in order after the first frames; a step may return a delay in ms before the next one. */
  function step(fn) { queue.push(fn); }
  function r4(el) { var r = el.getBoundingClientRect(); return [Math.round(r.left * 10) / 10, Math.round(r.top * 10) / 10, Math.round(r.width * 10) / 10, Math.round(r.height * 10) / 10]; }
  function measureTargets() {
    targetRects = {};
    for (var k in targets) { var el = typeof targets[k] === 'string' ? document.querySelector(targets[k]) : targets[k]; if (el) targetRects[k] = r4(el); }
  }
  function measure() {
    measureTargets();
    stageRects = {};
    var st = document.querySelectorAll('[data-stage]');
    for (var i = 0; i < st.length; i++) stageRects[st[i].getAttribute('data-stage')] = r4(st[i]);
  }
  function next() {
    if (!started && targetRects === null) { try { measure(); } catch (e) { record('harness', 'measure threw', false, e && e.message); } }
    if (!queue.length) { started = true; if (deadline) setTimeout(timeout, deadline); maybeFinish(); return; }
    var fn = queue.shift();
    var delay = 0;
    try { delay = fn() || 0; } catch (e) { record('harness', 'step threw', false, e && e.message); }
    setTimeout(next, delay);
  }

  /**
   * Start after the renderer had a chance to lay out (two frames, a fallback
   * timer when frames do not come). `awaitInput: ms` keeps the widget in
   * `awaiting` until its pending checks resolve or the time is up.
   */
  function run(opts) {
    opts = opts || {};
    if (opts.awaitInput) { awaiting = true; deadline = opts.awaitInput; }
    else if (opts.deadline) deadline = opts.deadline;
    publish();
    var went = false;
    var go = function () { if (!went) { went = true; setTimeout(next, opts.settle || 60); } };
    try { requestAnimationFrame(function () { requestAnimationFrame(go); }); } catch (e) {}
    setTimeout(go, 400);
  }

  // Reference mode (native rendering for the expected images): a stage with a
  // <template data-ref="name"> gets the template's markup instead of its own.
  function swapRefs() {
    var ts = document.querySelectorAll('template[data-ref]');
    for (var i = 0; i < ts.length; i++) {
      var name = ts[i].getAttribute('data-ref');
      var st = document.querySelector('[data-stage="' + name + '"]');
      if (st) { st.innerHTML = ts[i].innerHTML; st.setAttribute('data-ref-applied', ''); }
    }
  }
  /** Draw into a stage from script; skipped in reference mode when the stage has a template. */
  function stage(name, draw) {
    var el = document.querySelector('[data-stage="' + name + '"]');
    if (!el) return;
    if (mode === 'ref' && document.querySelector('template[data-ref="' + name + '"]')) return;
    try { draw(el); } catch (e) { record('harness', 'stage ' + name + ' threw', false, e && e.message); }
  }
  if (mode === 'ref') {
    if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', swapRefs); else swapRefs();
  }

  return {
    id: ID, sandbox: sandbox, mode: mode, check: check, eq: eq, near: near, wait: wait, ok: function (id, name, cond, detail, opts) { return check(id, name, function () { return [cond, detail]; }, opts); },
    rect: rect, cs: cs, color: color, step: step, run: run, stage: stage, publish: publish,
    /** A named element the runner may tap or drag (its rect is published as targets[name] when the run starts). */
    target: function (name, el) { targets[name] = el; },
    remeasure: function () { measure(); schedulePublish(); },
    skip: function (id, name, why) { return record(id, name, null, why); },
  };
})();
