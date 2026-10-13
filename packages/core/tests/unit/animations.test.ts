// Web Animations in the shim: the timing model runs in the sandbox, and each animation's state
// reaches the renderer as a validated `anim` record (src/sandbox/animations.ts,
// Validator.validateAnim).

import assert from 'node:assert/strict';
import { describe, it } from 'node:test';
import { runWidget } from '../../scripts/sandbox-node.ts';

const page = (script: string) => `<!DOCTYPE html><html><body><div id="box">x</div><div id="css">y</div><script>${script}</script></body></html>`;
const records = (r: Awaited<ReturnType<typeof runWidget>>) => r.anims.flatMap((m: any) => m.anims);
const errorsOf = (r: Awaited<ReturnType<typeof runWidget>>) => r.logs.filter((l) => l.level === 'error').map((l) => l.text);

describe('Element.animate', () => {
  it('sends a validated record after the ops that render its target', async () => {
    const r = await runWidget(page(`window.a = document.getElementById('box').animate([{ opacity: 0, backgroundColor: 'red' }, { opacity: 1, backgroundColor: 'blue' }], { duration: 400, iterations: Infinity, easing: 'ease-in-out' });`), {
      probes: ['a.playState', 'typeof a.currentTime', 'document.getAnimations().length'],
    });
    assert.deepEqual(errorsOf(r), []);
    const rec = records(r);
    assert.equal(rec.length, 1, JSON.stringify(r.anims));
    const box = r.renderOps.find((op: any) => op.op === 'el' && op.attrs?.some(([k, v]: string[]) => k === 'id' && v === 'box')) as any;
    assert.equal(rec[0].target, box.id);
    assert.deepEqual(rec[0].keyframes.map((k: any) => k.offset), [0, 1]);
    assert.equal(rec[0].keyframes[1].values['background-color'], 'blue');
    assert.equal(rec[0].timing.iterations, null);
    assert.equal(rec[0].timing.easing, 'ease-in-out');
    assert.equal(typeof rec[0].startTime, 'number');
    assert.deepEqual(r.probes, ['running', 'number', '1']);
  });

  it('property-indexed keyframes spread their offsets and merge equal ones', async () => {
    const r = await runWidget(page(`window.a = document.getElementById('box').animate({ opacity: [0, 0.5, 1], transform: ['none', 'translateX(10px)'] }, 300);`), {
      probes: ['JSON.stringify(a.effect.getKeyframes().map(k => [k.offset, k.opacity, k.transform]))'],
    });
    assert.deepEqual(JSON.parse(r.probes[0]), [[0, '0', 'none'], [0.5, '0.5', null], [1, '1', 'translateX(10px)']]);
  });

  it('pause holds the current time and sends a holdTime; play resumes from it', async () => {
    const r = await runWidget(page(`window.a = document.getElementById('box').animate({ opacity: [0, 1] }, 10000); a.pause(); a.currentTime = 2500;`), {
      script: [{ probe: 'a.playState + " " + a.currentTime' }, { probe: '(a.play(), a.playState)' }],
    });
    assert.deepEqual(r.probes.slice(0, 2), ['paused 2500', 'running']);
    const rec = records(r);
    assert.ok(rec.some((x: any) => x.holdTime === 2500), JSON.stringify(rec));
    assert.equal(typeof rec[rec.length - 1].startTime, 'number');
  });

  it('finish() fires finish and resolves finished; a non-filling finished animation is removed from the renderer', async () => {
    const r = await runWidget(page(`window.log = []; window.a = document.getElementById('box').animate({ opacity: [0, 1] }, 10000); a.onfinish = () => log.push('onfinish'); a.addEventListener('finish', () => log.push('event')); a.finished.then(() => log.push('promise'));`), {
      script: [{ probe: '(a.finish(), a.playState)' }, { probe: 'log.join(",")' }],
    });
    assert.equal(r.probes[0], 'finished');
    assert.equal(r.probes[1], 'onfinish,event,promise');
    const rec = records(r);
    assert.ok(rec.some((x: any) => x.remove), JSON.stringify(rec));
  });

  it('cancel() removes the record, goes idle and rejects finished with AbortError', async () => {
    const r = await runWidget(page(`window.out = ''; window.a = document.getElementById('box').animate({ opacity: [0, 1] }, { duration: 1000, fill: 'forwards' }); a.finished.catch(e => { out = e.name; }); setTimeout(() => a.cancel(), 0);`), {
      timerRounds: 2,
      probes: ['a.playState', 'out'],
    });
    assert.deepEqual(r.probes, ['idle', 'AbortError']);
    const rec = records(r);
    assert.ok(rec[0].keyframes && rec[rec.length - 1].remove, JSON.stringify(rec));
  });

  it('the validator drops animation-*, url() and bad easings', async () => {
    const r = await runWidget(page(`
      document.getElementById('box').animate([{ animationName: 'x', backgroundImage: 'url(https://evil.example/x.png)', opacity: 0 }, { opacity: 1 }], 100);
      try { document.getElementById('box').animate({ opacity: [0, 1] }, { duration: 100, easing: 'cubic-bezier(2, 0, 0, 1)' }); window.threw = ''; } catch (e) { window.threw = e.name; }`), { probes: ['threw'] });
    assert.equal(r.probes[0], 'TypeError', 'an out-of-range cubic-bezier throws, as in a browser');
    const rec = records(r);
    assert.equal(rec.length, 1);
    assert.deepEqual(Object.keys(rec[0].keyframes[0].values), ['opacity']);
  });

  it('getAnimations() returns CSS animations from the renderer and controls them with css records', async () => {
    const r = await runWidget(page(`window.list = document.getElementById('css').getAnimations();`), {
      answer: (q: any) => (q.q === 'anims' ? { list: [{ kind: 'animation', id: q.id, name: 'spin', index: 0, origin: 1234, currentTime: 500, playbackRate: 1, paused: false, duration: 1000, delay: 0, iterations: null, direction: 'normal', fill: 'none', easing: 'linear' }] } : undefined),
      script: [{ probe: 'list.length + " " + list[0].animationName + " " + list[0].playState' }, { probe: '(list[0].pause(), list[0].playState)' }],
    });
    assert.deepEqual(r.probes.slice(0, 2), ['1 spin running', 'paused']);
    const rec = records(r);
    const css = rec.find((x: any) => x.css);
    assert.ok(css && css.css.name === 'spin' && css.css.origin === 1234 && typeof css.holdTime === 'number', JSON.stringify(rec));
  });
});
